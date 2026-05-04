#!/usr/bin/env python3
"""Generate a NanoSVG-compatible test SVG.

NanoSVG ignores <text>, <use>, <style> blocks, and CSS classes, so we:
  - emit shapes via svgwrite (inline style attributes only)
  - flatten "Hello world" into a single <path> via matplotlib.textpath,
    avoiding the <defs>/<use> glyph references matplotlib's SVG backend
    would otherwise produce.

Deps: pip install svgwrite matplotlib
"""

import argparse

import svgwrite
from matplotlib.font_manager import FontProperties
from matplotlib.path import Path as MplPath
from matplotlib.textpath import TextPath


def textpath_to_d(tp: TextPath) -> str:
    parts = []
    for verts, code in tp.iter_segments(simplify=False):
        if code == MplPath.MOVETO:
            parts.append(f"M{verts[0]:.3f},{verts[1]:.3f}")
        elif code == MplPath.LINETO:
            parts.append(f"L{verts[0]:.3f},{verts[1]:.3f}")
        elif code == MplPath.CURVE3:
            parts.append(
                f"Q{verts[0]:.3f},{verts[1]:.3f} {verts[2]:.3f},{verts[3]:.3f}"
            )
        elif code == MplPath.CURVE4:
            parts.append(
                f"C{verts[0]:.3f},{verts[1]:.3f} "
                f"{verts[2]:.3f},{verts[3]:.3f} "
                f"{verts[4]:.3f},{verts[5]:.3f}"
            )
        elif code == MplPath.CLOSEPOLY:
            parts.append("Z")
    return " ".join(parts)


def generate(out_path: str, width: int = 800, height: int = 600) -> None:
    dwg = svgwrite.Drawing(
        out_path, size=(width, height), viewBox=f"0 0 {width} {height}"
    )

    dwg.add(dwg.rect(insert=(40, 40), size=(220, 140), fill="#e63946"))
    dwg.add(
        dwg.circle(center=(220, 160), r=90, fill="#1d3557", fill_opacity=0.5)
    )
    dwg.add(
        dwg.polygon(
            points=[(360, 60), (560, 60), (460, 220)],
            fill="#2a9d8f",
            fill_opacity=0.6,
        )
    )
    dwg.add(
        dwg.rect(
            insert=(600, 60),
            size=(160, 160),
            fill="none",
            stroke="#f1c40f",
            stroke_width=4,
        )
    )
    dwg.add(
        dwg.line(
            start=(40, 300),
            end=(width - 40, 300),
            stroke="#e76f51",
            stroke_width=3,
        )
    )
    dwg.add(
        dwg.ellipse(
            center=(width / 2, 470),
            r=(300, 90),
            fill="#c71585",
            fill_opacity=0.35,
        )
    )

    fp = FontProperties(family="sans-serif", weight="bold")
    tp = TextPath((0, 0), "Hello world", size=48, prop=fp)
    # TextPath uses font coords (y-up, baseline at y=0); SVG is y-down. Flip
    # via scale(1, -1) and translate to place the baseline at the desired y.
    dwg.add(
        dwg.path(
            d=textpath_to_d(tp),
            fill="#222222",
            transform="translate(200, 400) scale(1, -1)",
        )
    )

    dwg.save()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", "--output", default="/tmp/test.svg")
    ap.add_argument("--width", type=int, default=800)
    ap.add_argument("--height", type=int, default=600)
    args = ap.parse_args()
    generate(args.output, args.width, args.height)
    print(f"wrote {args.output}")


if __name__ == "__main__":
    main()
