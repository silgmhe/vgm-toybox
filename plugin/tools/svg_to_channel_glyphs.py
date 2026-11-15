#!/usr/bin/env python3
"""Turns the channel-type icons in `assets/icon/*.svg` into the C++ that draws them.

**The SVG is the design, this output is the drawing.** Editing a curve means editing the SVG and
running this script, so the diff says which line moved instead of hiding it in hand-tuned
coordinates. The generated file is committed (the build itself needs no Python).

What the script has to do beyond reading the path data:

- **Flatten the dense polylines.** `fm.svg` draws its two envelopes as ~240 straight segments each,
  which is 480 lines of `lineTo` nobody would read. Douglas-Peucker at `TOLERANCE` viewBox units
  brings them back to a couple of dozen points - at the 13-14 px these are drawn at, the difference
  is under a tenth of a pixel.
- **Keep the stroke relative.** The icons are 64 units wide with a 3-unit stroke, so a 13 px glyph
  would get a 0.6 px line and vanish. Every width is emitted as a fraction of the box with a 1 px
  floor, which a runtime SVG renderer could not do (the numbers there are fixed).
- **Resolve `currentColor`.** It is a CSS keyword the C++ knows nothing about; the caller passes the
  colour, which is how the icons follow the theme.

Run from the project root:  python plugin/tools/svg_to_channel_glyphs.py
"""

import math
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICON_DIR = os.path.join(ROOT, "assets", "icon")
OUT_PATH = os.path.join(ROOT, "Source", "ui", "ChannelGlyphs.generated.cpp")

# ViewBox units a point has to be off the line before it is kept. 0.12 of a 64-unit box is 0.19 %
# of the width - less than a pixel at any size this is drawn at, and enough to drop the samples
# `fm.svg` uses to spell out a sine.
TOLERANCE = 0.12

# (svg file, function name, enumeration name)
ICONS = [
    ("pulse", "pulse", "pulse"),
    ("triangle", "triangle", "triangle"),
    ("saw", "saw", "saw"),
    ("noise", "noise", "noise"),
    ("fm", "fm", "fm"),
    ("wavetable", "wavetable", "wavetable"),
    ("pcm", "pcm", "pcm"),
]

NUM = re.compile(r"[-+]?(?:\d*\.\d+|\d+\.?\d*)(?:[eE][-+]?\d+)?")


# --- path data -------------------------------------------------------------------------------

def tokenize(d):
    """`d` as a list of (command, [numbers]). Repeated commands with omitted letters ("M 1 2 3 4"
    meaning moveto then lineto) are left as one run and split by the caller, which is what the
    SVG spec asks for."""
    out = []
    i = 0
    while i < len(d):
        ch = d[i]
        if ch in "MmLlHhVvCcSsZz":
            i += 1
            nums = []
            while i < len(d) and d[i] not in "MmLlHhVvCcSsZz":
                m = NUM.match(d, i)
                if not m:
                    i += 1
                    continue
                nums.append(float(m.group()))
                i = m.end()
            out.append((ch, nums))
        else:
            i += 1
    return out


def to_absolute(d):
    """Absolute (command, points) runs. One run is one pen stroke: the moveto and every segment
    that follows it, up to the next moveto - so the caller can rebuild each as
    startNewSubPath() / lineTo() / cubicTo() on one `Path`.

    Accumulating into a single run is the point: emitting each command as its own run (what this
    did first) left every H/V as a one-point subpath, and the generated code drew dots instead of
    waveforms."""
    cx = cy = 0.0
    start_x = start_y = 0.0
    last_cubic_ctrl = None
    runs = []
    current = None

    def begin_if_needed():
        nonlocal current
        if current is None:
            current = [("M", cx, cy)]

    for cmd, nums in tokenize(d):
        upper = cmd.upper()
        rel = cmd.islower()
        i = 0

        if upper == "M":
            while i + 1 < len(nums):
                x, y = nums[i], nums[i + 1]
                if rel:
                    x += cx
                    y += cy
                cx, cy = x, y
                start_x, start_y = x, y

                if current is not None:
                    runs.append(current)

                # a moveto with extra pairs is a lineto, per the SVG spec
                current = [("M", x, y)] if i == 0 else [("L", x, y)]
                if i > 0:
                    current.insert(0, ("M", cx, cy))
                last_cubic_ctrl = None
                i += 2
        elif upper == "L":
            while i + 1 < len(nums):
                x, y = nums[i], nums[i + 1]
                if rel:
                    x += cx
                    y += cy
                cx, cy = x, y
                begin_if_needed()
                current.append(("L", x, y))
                last_cubic_ctrl = None
                i += 2
        elif upper == "H":
            for v in nums:
                x = v + cx if rel else v
                cx = x
                begin_if_needed()
                current.append(("L", x, cy))
                last_cubic_ctrl = None
        elif upper == "V":
            for v in nums:
                y = v + cy if rel else v
                cy = y
                begin_if_needed()
                current.append(("L", cx, y))
                last_cubic_ctrl = None
        elif upper == "C":
            while i + 5 < len(nums):
                pts = list(nums[i:i + 6])
                if rel:
                    for k in range(0, 6, 2):
                        pts[k] += cx
                        pts[k + 1] += cy
                c1x, c1y, c2x, c2y, x, y = pts
                cx, cy = x, y
                begin_if_needed()
                current.append(("C", c1x, c1y, c2x, c2y, x, y))
                last_cubic_ctrl = (c2x, c2y)
                i += 6
        elif upper == "S":
            while i + 3 < len(nums):
                pts = list(nums[i:i + 4])
                if rel:
                    for k in range(0, 4, 2):
                        pts[k] += cx
                        pts[k + 1] += cy
                c2x, c2y, x, y = pts
                # the smooth variant mirrors the previous control point
                if last_cubic_ctrl is None:
                    c1x, c1y = cx, cy
                else:
                    c1x, c1y = 2 * cx - last_cubic_ctrl[0], 2 * cy - last_cubic_ctrl[1]
                cx, cy = x, y
                begin_if_needed()
                current.append(("C", c1x, c1y, c2x, c2y, x, y))
                last_cubic_ctrl = (c2x, c2y)
                i += 4
        elif upper == "Z":
            if current is not None:
                current.append(("Z",))
            cx, cy = start_x, start_y
            last_cubic_ctrl = None

    if current is not None:
        runs.append(current)

    return runs


def simplify(points, tolerance):
    """Douglas-Peucker. `points` are (x, y) pairs; the ends are always kept."""
    if len(points) < 3:
        return points

    def dist(p, a, b):
        ax, ay = a
        bx, by = b
        px, py = p
        dx, dy = bx - ax, by - ay
        if dx == 0 and dy == 0:
            return math.hypot(px - ax, py - ay)
        t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
        t = max(0.0, min(1.0, t))
        return math.hypot(px - (ax + t * dx), py - (ay + t * dy))

    keep = [False] * len(points)
    keep[0] = keep[-1] = True
    stack = [(0, len(points) - 1)]
    while stack:
        lo, hi = stack.pop()
        worst, worst_i = 0.0, -1
        for i in range(lo + 1, hi):
            d = dist(points[i], points[lo], points[hi])
            if d > worst:
                worst, worst_i = d, i
        if worst > tolerance and worst_i > 0:
            keep[worst_i] = True
            stack.append((lo, worst_i))
            stack.append((worst_i, hi))

    return [p for p, k in zip(points, keep) if k]


def flatten_run(run):
    """A run of `L` segments into the points it visits, so a dense polyline can be simplified."""
    pts = []
    for seg in run:
        if seg[0] == "M":
            pts.append((seg[1], seg[2]))
        elif seg[0] == "L":
            pts.append((seg[1], seg[2]))
    return pts


# --- svg reading ------------------------------------------------------------------------------

def read_svg(name):
    with open(os.path.join(ICON_DIR, name + ".svg"), "r", encoding="utf-8") as f:
        raw = f.read()

    strokes = []   # dicts: {'width': float, 'opacity': float, 'runs': [...], 'circles': [...]}

    for m in re.finditer(r"<path\b([^>]*)/?>", raw):
        attrs = m.group(1)
        d_match = re.search(r'\bd="([^"]*)"', attrs)
        if not d_match:
            continue
        width = re.search(r'stroke-width="([\d.]+)"', attrs)
        opacity = re.search(r'\bopacity="([\d.]+)"', attrs)
        strokes.append({
            "width": float(width.group(1)) if width else 3.0,
            "opacity": float(opacity.group(1)) if opacity else 1.0,
            "runs": to_absolute(d_match.group(1)),
            "circles": [],
        })

    for m in re.finditer(r"<circle\b([^>]*)/?>", raw):
        attrs = m.group(1)
        cx = re.search(r'cx="([\d.]+)"', attrs)
        cy = re.search(r'cy="([\d.]+)"', attrs)
        r = re.search(r'\br="([\d.]+)"', attrs)
        if not (cx and cy and r):
            continue
        entry = {"cx": float(cx.group(1)), "cy": float(cy.group(1)), "r": float(r.group(1))}
        # a filled dot belongs to the path it follows in spirit, but drawing it is simpler if it
        # rides with the last stroke's group
        if strokes:
            strokes[-1]["circles"].append(entry)
        else:
            strokes.append({"width": 0.0, "opacity": 1.0, "runs": [], "circles": [entry]})

    return strokes


# --- code emission ----------------------------------------------------------------------------

def emit_icon(svg_name, function_name, enum_name):
    strokes = read_svg(svg_name)
    lines = []
    lines.append(f"void draw_{function_name}Glyph (juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)")
    lines.append("{")
    lines.append("\tconst float s = area.getWidth () / 64.0f;\t// the SVG's viewBox is 64 wide")
    lines.append("\tjuce::Path p;")
    lines.append("")

    for stroke in strokes:
        if not stroke["runs"] and not stroke["circles"]:
            continue

        alpha = stroke["opacity"]
        if alpha < 1.0:
            lines.append(f"\tg.setColour (colour.withAlpha ({alpha:.2f}f));")
        else:
            lines.append("\tg.setColour (colour);")

        for run in stroke["runs"]:
            kinds = {seg[0] for seg in run}

            if "C" in kinds:
                # A curve cannot be simplified point-wise: emit every command as written.
                for seg in run:
                    if seg[0] == "M":
                        lines.append(f"\tp.startNewSubPath ({_fmt(seg[1])} * s, {_fmt(seg[2])} * s);")
                    elif seg[0] == "L":
                        lines.append(f"\tp.lineTo ({_fmt(seg[1])} * s, {_fmt(seg[2])} * s);")
                    elif seg[0] == "C":
                        lines.append(
                            f"\tp.cubicTo ({_fmt(seg[1])} * s, {_fmt(seg[2])} * s, "
                            f"{_fmt(seg[3])} * s, {_fmt(seg[4])} * s, "
                            f"{_fmt(seg[5])} * s, {_fmt(seg[6])} * s);")
                    elif seg[0] == "Z":
                        lines.append("\tp.closeSubPath ();")
            else:
                pts = [(seg[1], seg[2]) for seg in run if seg[0] in ("M", "L")]
                if not pts:
                    continue

                simplified = simplify(pts, TOLERANCE)
                if len(simplified) != len(pts):
                    lines.append(f"\t// {len(pts)} sampled points, {len(simplified)} kept (see TOLERANCE)")

                lines.append(f"\tp.startNewSubPath ({_fmt(simplified[0][0])} * s, {_fmt(simplified[0][1])} * s);")
                for x, y in simplified[1:]:
                    lines.append(f"\tp.lineTo ({_fmt(x)} * s, {_fmt(y)} * s);")

                if "Z" in kinds:
                    lines.append("\tp.closeSubPath ();")

        for dot in stroke["circles"]:
            lines.append(f"\tg.fillEllipse ({_fmt(dot['cx'] - dot['r'])} * s, {_fmt(dot['cy'] - dot['r'])} * s,")
            lines.append(f"\t\t\t   {_fmt(dot['r'] * 2)} * s, {_fmt(dot['r'] * 2)} * s);")

        # A 3-unit stroke in a 64-unit box is 4.7 %: at 13 px that is 0.6 px, which is not a line
        # but a rumour of one. The floor is what keeps a small glyph legible.
        if stroke["width"] > 0:
            lines.append(f"\tg.strokePath (p, juce::PathStrokeType (juce::jmax (1.0f, "
                         f"{stroke['width']:.2f}f * s)));")
            lines.append("\tp.clear ();")

        lines.append("")

    while lines and lines[-1] == "":
        lines.pop()
    lines.append("}")
    return "\n".join(lines)


def _fmt(value):
    """Shortest form that keeps the drawing: whole numbers lose the .0."""
    if abs(value - round(value)) < 1e-6:
        return str(int(round(value)))
    return f"{value:.2f}"


def main():
    header = f'''// **GENERATED FILE - do not edit by hand.**
//
// Source of truth: plugin/assets/icon/*.svg
// Regenerate with: python plugin/tools/svg_to_channel_glyphs.py
//
// The script flattens the dense polylines the SVGs use to spell out curves, keeps the stroke
// widths relative to the 64-unit viewBox with a 1 px floor, and drops `currentColor` in favour of
// the caller's colour so the glyphs follow the theme.

#include "ChannelGlyphs.h"

namespace toybox::ui
{{

'''

    bodies = []
    for svg_name, function_name, _enum_name in ICONS:
        bodies.append(emit_icon(svg_name, function_name, _enum_name))
        bodies.append("")

    dispatch = ["void drawChannelGlyph (juce::Graphics& g, ChannelGlyph glyph,",
                "\t\t\t\t\t\t  juce::Rectangle<float> area, juce::Colour colour)",
                "{",
                "\t// The generated bodies draw in viewBox units from (0, 0); this puts that origin where",
                "\t// the caller asked without every one of them repeating the offset.",
                "\tjuce::Graphics::ScopedSaveState save (g);",
                "\tg.addTransform (juce::AffineTransform::translation (area.getX (), area.getY ()));",
                "",
                "\tswitch (glyph)",
                "\t{"]
    for _svg, function_name, enum_name in ICONS:
        dispatch.append(f"\tcase ChannelGlyph::{enum_name}:\tdraw_{function_name}Glyph (g, area, colour); break;")
    dispatch.append("\t}")
    dispatch.append("}")

    with open(OUT_PATH, "w", encoding="utf-8", newline="\n") as f:
        f.write(header)
        f.write("\n\n".join(bodies))
        f.write("\n\n" + "\n".join(dispatch) + "\n\n")
        f.write("} // namespace toybox::ui\n")

    print("wrote", os.path.relpath(OUT_PATH, ROOT))


if __name__ == "__main__":
    main()
