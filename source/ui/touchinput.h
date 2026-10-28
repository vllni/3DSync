#ifndef UI_TOUCHINPUT_H
#define UI_TOUCHINPUT_H

#include <stdint.h>
#include <vector>

#include "screens.h"

// ---------------------------------------------------------------------------
// TouchInput  — the touch screen as a second set of buttons
// ---------------------------------------------------------------------------
// Turns the stylus into the same key bits the physical buttons produce, so the
// frontend handles one stream of keys and the two can never disagree:
//
//  - an ordinary button fires on release, and only if the stylus is still on
//    it, which is how a mis-tap is taken back;
//  - a repeating button (the scroll arrows) fires on contact and then repeats
//    while held, at the same rate as a held D-pad;
//  - the scroll track reports where along it the stylus is, while held.
//
// Fed once per frame.  Host-portable, so the tests drive it with coordinates.
// ---------------------------------------------------------------------------
namespace ui
{

// Frames before a held key starts repeating, and between repeats; frontend.cpp
// gives hidSetRepeatParameters() the same numbers.
const int REPEAT_DELAY = 18;
const int REPEAT_INTERVAL = 4;

struct TouchResult
{
    uint32_t pressed; // keys that fire this frame
    uint32_t held;    // key under a stylus resting on its button, for drawing
    bool dragging;    // stylus is on the scroll track
    float dragFraction;
};

class TouchInput
{
public:
    TouchInput();

    TouchResult update(bool touching, int x, int y, const std::vector<Button> &buttons);

    // Forget a touch in progress; called when the buttons change underneath
    // it, so a tap that began on "Sync now" cannot land on what replaced it.
    void cancel();

private:
    enum State
    {
        IDLE,
        ON_BUTTON,
        ON_TRACK,
        IGNORED // started on nothing, or cancelled: wait for release
    };
    State _state;
    uint32_t _key;
    Rect _rect;
    bool _repeats;
    int _frames;
    bool _inside;
};

} // namespace ui

#endif
