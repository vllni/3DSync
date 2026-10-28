#ifndef UI_SCREENS_H
#define UI_SCREENS_H

#include <stdint.h>
#include <string>
#include <vector>

#include "canvas.h"
#include "logbuffer.h"

// ---------------------------------------------------------------------------
// The two screens: layouts, drawing and touch targets
// ---------------------------------------------------------------------------
// Top: the scrollable log.  Bottom: what the app is doing, the actions open to
// the user right now as touch buttons, and a scroll strip for the log.
//
// Every touch button stands for a physical button and carries its key bit:
// tapping "Keep 3DS" is pressing A, and the frontend feeds both through the
// same handler.  The label always shows which key that is, so the touch screen
// teaches the buttons instead of replacing them.
//
// Host-portable: the frontend fills in a BottomModel, and the tests and the
// preview tool can do the same without a console.
// ---------------------------------------------------------------------------
namespace ui
{

// Screen sizes, landscape.
const int TOP_W = 400, TOP_H = 240;
const int BOTTOM_W = 320, BOTTOM_H = 240;

// Key bits, numerically identical to libctru's KEY_* (frontend.cpp checks).
const uint32_t K_A = 1u << 0;
const uint32_t K_B = 1u << 1;
const uint32_t K_SELECT = 1u << 2;
const uint32_t K_START = 1u << 3;
const uint32_t K_DRIGHT = 1u << 4;
const uint32_t K_DLEFT = 1u << 5;
const uint32_t K_DUP = 1u << 6;
const uint32_t K_DDOWN = 1u << 7;
const uint32_t K_R = 1u << 8;
const uint32_t K_L = 1u << 9;
const uint32_t K_X = 1u << 10;
const uint32_t K_Y = 1u << 11;

// The top-screen log: header, then LOG_ROWS rows of the mono font, whose
// every glyph advances LOG_ADVANCE pixels.  That works out at 54 columns,
// close to the 50 of the libctru console the app's messages were written for.
const int LOG_HEADER_H = 20;
const int LOG_ROW_H = 13;
const int LOG_ADVANCE = 7;
const int LOG_X = 6;
const int LOG_SCROLLBAR_W = 4;
const int LOG_ROWS = (TOP_H - LOG_HEADER_H - 4) / LOG_ROW_H;
const int LOG_COLUMNS = (TOP_W - 2 * LOG_X - LOG_SCROLLBAR_W - 2) / LOG_ADVANCE;

enum ScreenMode
{
    SCREEN_MENU,     // waiting for A / Y / START
    SCREEN_RUNNING,  // a sync is in progress
    SCREEN_CONFLICT, // the engine is waiting for a conflict answer
    SCREEN_SAVE_LOG  // end of a debug run: keep the transcript or not
};

struct BottomModel
{
    ScreenMode mode;
    bool debugRun;

    // SCREEN_MENU
    bool starting; // before the configuration has been read: buttons inert
    bool configLoaded;
    std::string remotes; // "Dropbox, SMB"
    int pathCount;
    std::string fileFilter; // "all files" or ".sav .bin"; "" reads as all
    std::string lastResult; // "" before the first run
    bool lastResultOk;

    // SCREEN_RUNNING
    std::string provider;
    std::string entry;
    unsigned elapsedSeconds;
    unsigned spinnerFrame;
    bool cancelling;

    // SCREEN_CONFLICT
    std::string conflictPath;
    bool applyToAll;

    // SCREEN_SAVE_LOG
    unsigned logKilobytes;
    bool logTrimmed;

    // Keys currently down, physically or by touch: drawn pressed.
    uint32_t heldKeys;

    // Log position, for the thumb on the scroll strip.
    size_t logFirst, logEnd, logTop;
    int logVisible;

    BottomModel();
};

enum ButtonStyle
{
    BUTTON_NORMAL,
    BUTTON_PRIMARY,
    BUTTON_DANGER,
    BUTTON_TOGGLE // drawn lit while BottomModel::applyToAll
};

struct Button
{
    Rect rect;
    uint32_t key;
    std::string label;
    ButtonStyle style;
    bool enabled;
    // Fires on touch-down and repeats while held, like a held D-pad;
    // otherwise a tap fires on release over the same button.
    bool repeats;
};

// The scroll strip on the right of the bottom screen: ▲ and ▼ are touch
// buttons for D-pad Up and Down, and the track between them can be dragged.
const Rect STRIP_UP = {282, 36, 34, 34};
const Rect STRIP_TRACK = {282, 74, 34, 126};
const Rect STRIP_DOWN = {282, 204, 34, 32};

// Every touch target for this model, the scroll arrows included.
std::vector<Button> bottomButtons(const BottomModel &m);

// The button under (x, y), or -1.
int hitButton(const std::vector<Button> &buttons, int x, int y);

void drawBottom(Canvas &c, const BottomModel &m, const std::vector<Button> &buttons);

struct TopModel
{
    bool debugRun;
    bool busy; // a sync is running
};

void drawTop(Canvas &c, const TopModel &m, const LogBuffer &log, const LogScroll &scroll);

} // namespace ui

#endif
