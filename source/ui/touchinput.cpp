#include "touchinput.h"

namespace ui
{

TouchInput::TouchInput()
    : _state(IDLE), _key(0), _repeats(false), _frames(0), _inside(false)
{
    _rect.x = _rect.y = _rect.w = _rect.h = 0;
}

void TouchInput::cancel()
{
    if (_state != IDLE)
        _state = IGNORED;
}

static float trackFraction(int y)
{
    // The thumb centre follows the stylus, so the ends of the track are
    // reachable without pressing on its very edge.
    const int margin = 12;
    float f = (float)(y - STRIP_TRACK.y - margin) / (STRIP_TRACK.h - 2 * margin);
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}

TouchResult TouchInput::update(bool touching, int x, int y, const std::vector<Button> &buttons)
{
    TouchResult r;
    r.pressed = 0;
    r.held = 0;
    r.dragging = false;
    r.dragFraction = 0;

    if (!touching)
    {
        if (_state == ON_BUTTON && !_repeats && _inside)
            r.pressed = _key;
        _state = IDLE;
        return r;
    }

    if (_state == IDLE)
    {
        int hit = hitButton(buttons, x, y);
        if (hit >= 0)
        {
            _state = ON_BUTTON;
            _key = buttons[hit].key;
            _rect = buttons[hit].rect;
            _repeats = buttons[hit].repeats;
            _frames = 0;
            _inside = true;
            if (_repeats)
                r.pressed = _key;
        }
        else if (STRIP_TRACK.contains(x, y))
            _state = ON_TRACK;
        else
            _state = IGNORED;
    }
    else if (_state == ON_BUTTON)
    {
        _inside = _rect.contains(x, y);
        _frames++;
        if (_repeats && _inside && _frames >= REPEAT_DELAY &&
            (_frames - REPEAT_DELAY) % REPEAT_INTERVAL == 0)
            r.pressed = _key;
    }

    if (_state == ON_BUTTON && _inside)
        r.held = _key;
    if (_state == ON_TRACK)
    {
        r.dragging = true;
        r.dragFraction = trackFraction(y);
    }
    return r;
}

} // namespace ui
