#!/usr/bin/env python3
"""Draws the benchmark chart of the README, docs/benchmark-light.svg and
docs/benchmark-dark.svg, from the results that bench/run.py writes with --json: the run times
of one program, on a linear axis; bars past the end of the axis are cut.

Usage: docs/make_chart.py <results.json> [program] [output-directory]
"""

import json
import os
import sys

IMPLEMENTATIONS = ["Pietric", "PietCC", "rpiet", "npiet", "piet_programming_language"]
TITLES = {
    "bf-squares": "Brainfuck interpreter written in Piet, running squares.b",
}
AXIS_END = 15  # seconds
THEMES = {
    # GitHub's colors; "page" is the background behind the image, for the cuts in long bars.
    "light": {"text": "#1f2328", "muted": "#59636e", "grid": "#d0d7de", "bar": "#8c959f",
              "ours": "#1a7f37", "page": "#ffffff"},
    "dark": {"text": "#f0f6fc", "muted": "#9198a1", "grid": "#3d444d", "bar": "#656c76",
             "ours": "#3fb950", "page": "#0d1117"},
}


def figure(seconds):
    """Seconds written as in the README's table: 0.095, 5.5, 12.3, 144."""
    if seconds < 1:
        return "%.3f" % seconds
    return "%.1f" % seconds if seconds < 100 else "%.0f" % seconds


def chart(bars, title, theme):
    c = THEMES[theme]
    width, left, right, top, bar_height, gap = 760, 220, 70, 64, 30, 12
    height = top + len(bars) * (bar_height + gap) + 36
    x = lambda seconds: left + (width - left - right) * min(seconds, AXIS_END) / AXIS_END
    out = ['<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" '
           'viewBox="0 0 %d %d" font-family="-apple-system, Segoe UI, Helvetica, Arial, '
           'sans-serif">' % (width, height, width, height),
           '<text x="0" y="24" font-size="17" font-weight="600" fill="%s">%s</text>'
           % (c["text"], title),
           '<text x="0" y="46" font-size="13" fill="%s">run time in seconds, lower is better'
           '</text>' % c["muted"]]
    for tick in range(0, AXIS_END + 1, 5):
        out.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s"/>'
                   % (x(tick), top - 4, x(tick), height - 30, c["grid"]))
        out.append('<text x="%.1f" y="%d" font-size="12" fill="%s" text-anchor="middle">%d'
                   '</text>' % (x(tick), height - 12, c["muted"], tick))
    for i, (name, seconds, optimized) in enumerate(bars):
        y = top + i * (bar_height + gap)
        ours = name == "Pietric"
        label = name
        out.append('<text x="%d" y="%d" font-size="14" fill="%s" text-anchor="end"%s>%s</text>'
                   % (left - 12, y + bar_height / 2 + 5, c["text"],
                      ' font-weight="600"' if ours else "", label))
        end = x(seconds)
        out.append('<rect x="%d" y="%d" width="%.1f" height="%d" rx="3" fill="%s"/>'
                   % (left, y, max(end - left, 2), bar_height, c["ours"] if ours else c["bar"]))
        if seconds > AXIS_END:
            # A wavy cut near the end: the bar goes on beyond the axis.
            cut = end - 26
            wave = " ".join("%.1f,%.1f" % (cut + (4 if k % 2 else -4), y - 3 + k * (bar_height + 6) / 6)
                            for k in range(7))
            out.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="7"/>'
                       % (wave, c["page"]))
        # As in the README's table, † marks PietCC without --o2.
        out.append('<text x="%.1f" y="%d" font-size="13" fill="%s"%s>%s%s s</text>'
                   % (end + 8, y + bar_height / 2 + 5, c["text"],
                      ' font-weight="600"' if ours else "", "" if optimized else "† ",
                      figure(seconds)))
    out.append("</svg>")
    return "\n".join(out) + "\n"


def main():
    results = json.load(open(sys.argv[1]))
    program = sys.argv[2] if len(sys.argv) > 2 else "bf-squares"
    directory = sys.argv[3] if len(sys.argv) > 3 else "docs"
    row = results["run"][program]
    bars = sorted(((name, row[name]["time"], row[name].get("optimized", True))
                   for name in IMPLEMENTATIONS if "time" in row.get(name, {})),
                  key=lambda bar: bar[1])
    for theme in THEMES:
        with open(os.path.join(directory, "benchmark-%s.svg" % theme), "w") as f:
            f.write(chart(bars, TITLES.get(program, program), theme))


if __name__ == "__main__":
    main()
