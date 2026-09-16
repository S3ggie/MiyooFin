#!/usr/bin/env python3
"""MiyooFin UI-harness screenshot assertions (test-only, stdlib only).

Reads the 32-bit BMP framebuffer captures produced by the app's screenshot
hook and applies coarse, robust checks -- deliberately NOT pixel-exact
goldens (those would be brittle across themes/fonts):

  rendered  frame is not blank/single-colour (catches "nothing rendered")
  rails     the list/grid band is populated, not empty/uniform
            (catches the grey-poster / "no seasons" class of bug)
  seasons   the lower list band on a Series screen is populated

Usage:
  assert_shots.py --shot <file.bmp> --checks rendered,rails

Exit 0 when all checks pass, 1 otherwise, with one line per check plus
diagnostic stats. Thresholds are coarse on purpose; see
docs/ui-script-harness.md.
"""
import argparse
import struct
import sys

W, H = 640, 480

# Coarse thresholds (calibrated against real captures; see docs).
# Populated Home: distinct=11 dominant=0.635; empty Home: distinct=4
# dominant=0.911. Rails band populated: distinct=8 bright=0.013; empty
# rails: distinct=2 bright=0.0024.
MIN_DISTINCT_RENDERED = 8
MAX_DOMINANT_RENDERED = 0.88
MIN_DISTINCT_RAILS = 5
MIN_BRIGHT_RAILS = 0.006
MIN_DISTINCT_SEASONS = 5
MIN_BRIGHT_SEASONS = 0.004
BRIGHT_LUM = 170


def load_bmp(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[0:2] != b"BM":
        raise ValueError("not a BMP file")
    (px_off,) = struct.unpack_from("<I", data, 10)
    (w, h, planes, bpp, comp) = struct.unpack_from("<iiHHI", data, 18)
    # SDL_SaveBMP writes 32-bit framebuffers as BI_BITFIELDS (comp=3) with
    # RGBA masks; still 4 bytes/pixel, same layout as BI_RGB for our reads.
    if w != W or h not in (H, -H) or bpp != 32 or comp not in (0, 3):
        raise ValueError("unexpected BMP format %dx%d bpp=%d comp=%d"
                         % (w, h, bpp, comp))
    top_down = h < 0
    h = abs(h)
    stride = w * 4
    px = []
    for row in range(h):
        src = row if top_down else (h - 1 - row)
        off = px_off + src * stride
        px.extend(struct.unpack_from("<%dI" % w, data, off))
    return px  # 0xAARRGGBB little-endian words -> low byte is Blue


def stats(px):
    hist = {}
    bright = 0
    for p in px:
        b = p & 0xFF
        g = (p >> 8) & 0xFF
        r = (p >> 16) & 0xFF
        hist[(r >> 3, g >> 3, b >> 3)] = hist.get((r >> 3, g >> 3, b >> 3), 0) + 1
        if (r + g + b) // 3 >= BRIGHT_LUM:
            bright += 1
    n = len(px)
    dom = max(hist.values()) / n if hist else 1.0
    return {"distinct": len(hist), "dominant": dom,
            "bright_frac": bright / n if n else 0.0, "pixels": n}


def band(px, y0, y1):
    out = []
    for y in range(max(0, y0), min(H, y1)):
        out.extend(px[y * W:(y + 1) * W])
    return out


def check_rendered(px):
    s = stats(px)
    ok = s["distinct"] >= MIN_DISTINCT_RENDERED \
        and s["dominant"] <= MAX_DOMINANT_RENDERED
    return ok, ("distinct=%d (>=%d) dominant=%.3f (<=%.2f)"
                % (s["distinct"], MIN_DISTINCT_RENDERED,
                   s["dominant"], MAX_DOMINANT_RENDERED))


def check_rails(px):
    s = stats(band(px, 150, 400))
    ok = s["distinct"] >= MIN_DISTINCT_RAILS \
        and s["bright_frac"] >= MIN_BRIGHT_RAILS
    return ok, ("distinct=%d (>=%d) bright=%.4f (>=%.4f)"
                % (s["distinct"], MIN_DISTINCT_RAILS,
                   s["bright_frac"], MIN_BRIGHT_RAILS))


def check_seasons(px):
    s = stats(band(px, 150, 460))
    ok = s["distinct"] >= MIN_DISTINCT_SEASONS \
        and s["bright_frac"] >= MIN_BRIGHT_SEASONS
    return ok, ("distinct=%d (>=%d) bright=%.4f (>=%.4f)"
                % (s["distinct"], MIN_DISTINCT_SEASONS,
                   s["bright_frac"], MIN_BRIGHT_SEASONS))


CHECKS = {"rendered": check_rendered, "rails": check_rails,
          "seasons": check_seasons}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shot", required=True)
    ap.add_argument("--checks", required=False, default="",
                    help="comma-separated: rendered,rails,seasons")
    ap.add_argument("--stats", action="store_true",
                    help="print full-frame stats and exit 0")
    args = ap.parse_args()
    try:
        px = load_bmp(args.shot)
    except (OSError, ValueError) as e:
        print("ERROR cannot load %s: %s" % (args.shot, e))
        return 1
    if args.stats:
        print("stats %s" % stats(px))
        return 0
    failed = 0
    for name in args.checks.split(","):
        name = name.strip()
        fn = CHECKS.get(name)
        if not fn:
            print("ERROR unknown check '%s'" % name)
            return 1
        ok, detail = fn(px)
        print("%s %s: %s" % ("PASS" if ok else "FAIL", name, detail))
        failed += 0 if ok else 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
