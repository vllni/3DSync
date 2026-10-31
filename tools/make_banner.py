#!/usr/bin/env python3
"""Draws meta/banner_3ds.png, the 256x128 HOME Menu banner.

    tools/make_banner.py <bold .ttf/.otf> meta/banner_3ds.png

Two arrows, blue over the top and red along the bottom, run round the name
as one loop, each head chasing the other's tail: the same blue/red pair as
meta/icon_3ds.png, closed into a cycle around "3DSync".  Drawn at 4x and
scaled down, so the curves come out anti-aliased.  The current banner uses
Lato Black (OFL, https://www.latofonts.com; Debian's fonts-lato).

Needs Pillow.
"""
import math
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 256, 128
S = 4  # supersampling

BLUE = (24, 40, 255, 255)
RED = (255, 24, 24, 255)
BLACK = (0, 0, 0, 255)
WHITE = (255, 255, 255, 255)

# The loop: a stadium around the name, in banner pixels.
LEFT, TOP, RIGHT, BOTTOM = 12, 14, 244, 110
RADIUS = (BOTTOM - TOP) / 2
STROKE = 13
HEAD_LEN, HEAD_W = 22, 30
GAP = 0.035  # of the loop's length, between a head and the next tail
SHADOW = (3, 3)


def loop_point(t):
    """A point on the stadium at t in [0, 1), from the middle of the right
    side, anticlockwise.  Returns (x, y) in banner pixels."""
    straight = RIGHT - LEFT - 2 * RADIUS
    arc = math.pi * RADIUS
    total = 2 * straight + 2 * arc
    d = (t % 1.0) * total
    cy = (TOP + BOTTOM) / 2
    # Right half circle, upper part: from the right middle up to the top.
    segments = [
        ("arc", RIGHT - RADIUS, cy, 0.0, math.pi / 2, arc / 2),
        ("line", RIGHT - RADIUS, TOP, LEFT + RADIUS, TOP, straight),
        ("arc", LEFT + RADIUS, cy, math.pi / 2, 3 * math.pi / 2, arc),
        ("line", LEFT + RADIUS, BOTTOM, RIGHT - RADIUS, BOTTOM, straight),
        ("arc", RIGHT - RADIUS, cy, 3 * math.pi / 2, 2 * math.pi, arc / 2),
    ]
    for seg in segments:
        length = seg[-1]
        if d <= length or seg is segments[-1]:
            f = d / length if length else 0
            if seg[0] == "line":
                _, x0, y0, x1, y1, _ = seg
                return x0 + (x1 - x0) * f, y0 + (y1 - y0) * f
            _, cx, ccy, a0, a1, _ = seg
            a = a0 + (a1 - a0) * f
            return cx + RADIUS * math.cos(a), ccy - RADIUS * math.sin(a)
        d -= length
    raise AssertionError


def arrow(draw, t0, t1, colour, offset=(0, 0)):
    """An arrow along the loop from t0 to t1, its head at t1."""
    ox, oy = offset
    steps = 400
    head_t = HEAD_LEN / (2 * (RIGHT - LEFT - 2 * RADIUS) + 2 * math.pi * RADIUS)
    r = STROKE * S / 2
    for i in range(steps + 1):
        t = t0 + (t1 - head_t - t0) * i / steps
        x, y = loop_point(t)
        x, y = (x + ox) * S, (y + oy) * S
        draw.ellipse((x - r, y - r, x + r, y + r), fill=colour)
    # The head: a triangle along the tangent at its base.
    bx, by = loop_point(t1 - head_t)
    tx, ty = loop_point(t1)
    dx, dy = tx - bx, ty - by
    n = math.hypot(dx, dy)
    dx, dy = dx / n, dy / n
    px, py = -dy, dx
    tip = (tx + dx * 2, ty + dy * 2)
    pts = [tip, (bx + px * HEAD_W / 2, by + py * HEAD_W / 2), (bx - px * HEAD_W / 2, by - py * HEAD_W / 2)]
    draw.polygon([((x + ox) * S, (y + oy) * S) for x, y in pts], fill=colour)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    font_path, out = sys.argv[1], sys.argv[2]

    img = Image.new("RGBA", (W * S, H * S), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # Blue runs over the top from the right to the left side, red along the
    # bottom from the left to the right: one anticlockwise cycle.
    blue = (0.0 + GAP / 2, 0.5 - GAP / 2)
    red = (0.5 + GAP / 2, 1.0 - GAP / 2)
    for t0, t1 in (blue, red):
        arrow(draw, t0, t1, BLACK, SHADOW)
    arrow(draw, *blue, BLUE)
    arrow(draw, *red, RED)

    # The name: black, outlined in white, with a black drop shadow.
    font = ImageFont.truetype(font_path, 50 * S)
    text = "3DSync"
    box = draw.textbbox((0, 0), text, font=font, stroke_width=3 * S)
    tw, th = box[2] - box[0], box[3] - box[1]
    x = (W * S - tw) / 2 - box[0]
    y = (H * S - th) / 2 - box[1]
    draw.text((x + SHADOW[0] * S, y + SHADOW[1] * S), text, font=font, fill=BLACK,
              stroke_width=3 * S, stroke_fill=BLACK)
    draw.text((x, y), text, font=font, fill=BLACK, stroke_width=3 * S, stroke_fill=WHITE)

    img = img.resize((W, H), Image.LANCZOS)
    img.save(out)


if __name__ == "__main__":
    main()
