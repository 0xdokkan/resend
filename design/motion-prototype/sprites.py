"""Draws the prototype's sprites as indexed images: python3 sprites.py palettes.json sprites.json sheet.png

Each sprite is a grid of roles, not of colours, so one drawing serves every palette.
"""
import json
import sys
from pathlib import Path
from PIL import Image

TONES = ("dd", "d", "", "l")
ROLES = ["out", "screen", "metal-dd", "metal-d", "metal", "metal-l"]
for family in ("ok", "link", "ask", "route", "acc"):
    ROLES += [f"{family}-{t}" if t else family for t in TONES]
ROLES += ["ink", "well"]
CHARS = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"


class Sprite:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.g = [[None] * w for _ in range(h)]

    def px(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.g[y][x] = ROLES.index(c)

    def pts(self, points, c):
        for x, y in points:
            self.px(x, y, c)

    def rect(self, x0, y0, x1, y1, c):
        for y in range(y0, y1 + 1):
            for x in range(x0, x1 + 1):
                self.px(x, y, c)

    def hline(self, x0, x1, y, c):
        self.rect(x0, y, x1, y, c)

    def vline(self, x, y0, y1, c):
        self.rect(x, y0, x, y1, c)

    def clear(self, points):
        for x, y in points:
            self.g[y][x] = None

    def outline(self, c="out"):
        """A one-pixel outline around everything drawn so far."""
        src = [row[:] for row in self.g]
        for y in range(self.h):
            for x in range(self.w):
                if src[y][x] is not None:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < self.w and 0 <= ny < self.h and src[ny][nx] is not None:
                        self.px(x, y, c)
                        break

    def over(self, other):
        for y in range(self.h):
            for x in range(self.w):
                if other.g[y][x] is not None:
                    self.g[y][x] = other.g[y][x]

    def text(self):
        return "".join("." if v is None else CHARS[v] for row in self.g for v in row)


def host32():
    s = Sprite(32, 32)
    s.rect(5, 3, 26, 19, "metal")
    s.hline(5, 26, 3, "metal-l")
    s.vline(5, 3, 19, "metal-l")
    s.hline(6, 26, 19, "metal-d")
    s.vline(26, 4, 19, "metal-d")
    s.clear([(5, 3), (26, 3), (5, 19), (26, 19)])
    s.rect(7, 5, 24, 16, "out")
    s.rect(8, 6, 23, 15, "screen")
    s.pts([(10, 8), (11, 9), (12, 10), (11, 11), (10, 12)], "ok")
    s.pts([(14, 12), (15, 12), (16, 12)], "ok")
    s.pts([(10, 14), (11, 14), (13, 14), (14, 14), (15, 14), (17, 14)], "ok-d")
    s.pts([(21, 7), (22, 7), (22, 8)], "metal-d")
    s.px(23, 18, "ok")
    s.px(21, 18, "metal-d")
    s.rect(14, 20, 17, 22, "metal-d")
    s.rect(10, 23, 21, 24, "metal")
    s.hline(10, 21, 23, "metal-l")
    s.outline()
    k = Sprite(32, 32)
    k.rect(6, 28, 25, 29, "metal-l")
    k.hline(6, 25, 29, "metal")
    k.pts([(x, 28) for x in range(7, 25, 2)], "metal-d")
    k.pts([(x, 29) for x in range(8, 25, 2)], "metal-d")
    k.outline()
    s.over(k)
    return s


def switch32():
    s = Sprite(32, 32)
    for i, y in enumerate(range(9, 13)):
        s.hline(6 - i, 25 + i, y, "link-d")
    s.hline(6, 25, 9, "link")
    s.rect(2, 13, 29, 23, "metal-d")
    s.hline(2, 29, 13, "metal")
    s.hline(2, 29, 23, "metal-dd")
    s.hline(3, 28, 14, "link-dd")
    for i in range(8):
        x = 4 + 3 * i
        s.hline(x, x + 1, 16, "link" if i < 2 else "metal")
        s.rect(x, 18, x + 1, 20, "out")
        s.hline(x, x + 1, 21, "metal")
    s.px(28, 16, "ok")
    s.outline()
    s.hline(4, 6, 25, "out")
    s.hline(25, 27, 25, "out")
    return s


def router32():
    s = Sprite(32, 32)
    for i, y in enumerate(range(7, 11)):
        s.hline(8 - i, 23 + i, y, "route-d")
    s.hline(8, 23, 7, "route")
    s.rect(4, 11, 27, 24, "metal-d")
    s.hline(4, 27, 11, "metal")
    s.hline(4, 27, 24, "metal-dd")
    s.hline(6, 12, 14, "route")
    s.pts([(11, 13), (11, 15), (10, 12), (10, 16)], "route")
    s.hline(6, 12, 19, "route")
    s.pts([(7, 18), (7, 20), (8, 17), (8, 21)], "route")
    for i in range(4):
        x = 15 + 3 * i
        s.hline(x, x + 1, 14, "link" if i < 2 else "metal")
        s.rect(x, 16, x + 1, 18, "out")
        s.hline(x, x + 1, 19, "metal")
    s.hline(14, 26, 22, "route-dd")
    s.outline()
    s.hline(6, 8, 26, "out")
    s.hline(23, 25, 26, "out")
    return s


def host16():
    s = Sprite(16, 16)
    s.rect(3, 3, 12, 9, "metal")
    s.hline(3, 12, 3, "metal-l")
    s.rect(4, 4, 11, 8, "screen")
    s.pts([(5, 5), (6, 6), (5, 7)], "ok")
    s.pts([(8, 7), (9, 7)], "ok")
    s.rect(7, 10, 8, 11, "metal-d")
    s.rect(5, 12, 10, 12, "metal-l")
    s.outline()
    return s


def switch16():
    s = Sprite(16, 16)
    s.hline(4, 11, 5, "link")
    s.hline(3, 12, 6, "link-d")
    s.rect(2, 7, 13, 11, "metal-d")
    s.hline(2, 13, 7, "metal")
    s.pts([(x, 9) for x in (3, 5, 7, 9, 11)], "link")
    s.pts([(x, 10) for x in (3, 5, 7, 9, 11)], "out")
    s.outline()
    return s


def router16():
    s = Sprite(16, 16)
    s.hline(5, 10, 4, "route")
    s.hline(4, 11, 5, "route-d")
    s.rect(3, 6, 12, 12, "metal-d")
    s.hline(3, 12, 6, "metal")
    s.hline(4, 9, 8, "route")
    s.pts([(8, 7), (10, 8)], "route")
    s.hline(6, 11, 10, "route")
    s.pts([(7, 11), (5, 10)], "route")
    s.outline()
    return s


def bubble(tail_right):
    s = Sprite(16, 16)
    s.rect(2, 2, 13, 10, "ask")
    s.hline(3, 12, 2, "ask-l")
    s.hline(3, 12, 10, "ask-d")
    s.clear([(2, 2), (13, 2), (2, 10), (13, 10)])
    if tail_right:
        s.pts([(11, 11), (10, 11), (11, 12)], "ask-d")
    else:
        s.pts([(4, 11), (5, 11), (4, 12)], "ask-d")
    return s


def arp_request16():
    """An ARP request: a question in a speech bubble."""
    s = bubble(False)
    s.pts([(6, 3), (7, 3), (8, 3), (5, 4), (9, 4), (9, 5), (8, 6), (7, 7), (7, 9)], "out")
    s.outline()
    return s


def arp_reply16():
    """An ARP reply: the answer, with the tail on the other side."""
    s = bubble(True)
    s.rect(7, 3, 8, 6, "out")
    s.rect(7, 8, 8, 8, "out")
    s.outline()
    return s


def ball():
    s = Sprite(16, 16)
    rows = {3: (6, 9), 4: (4, 11), 5: (4, 11), 6: (3, 12), 7: (3, 12), 8: (3, 12),
            9: (3, 12), 10: (4, 11), 11: (4, 11), 12: (6, 9)}
    for y, (x0, x1) in rows.items():
        s.hline(x0, x1, y, "ok")
    s.pts([(12, 8), (12, 9), (11, 9), (11, 10), (11, 11), (10, 10), (10, 11), (9, 11)], "ok-d")
    s.hline(6, 9, 12, "ok-d")
    return s


def echo_request16():
    """An Echo Request: a ball."""
    s = ball()
    s.pts([(5, 5), (6, 5), (5, 6), (6, 4)], "ok-l")
    s.outline()
    return s


def echo_reply16():
    """An Echo Reply: the ball comes back as a ring."""
    s = ball()
    s.pts([(5, 5), (6, 4), (7, 4)], "ok-l")
    s.rect(6, 6, 9, 9, "out")
    s.clear([(7, 7), (8, 7), (7, 8), (8, 8)])
    s.outline()
    s.clear([(7, 7), (8, 7), (7, 8), (8, 8)])
    return s


def icon(points, c="ink"):
    s = Sprite(16, 16)
    s.pts(points, c)
    return s


def play16():
    pts = []
    for r in range(10):
        w = 2 * (min(r, 9 - r) + 1)
        pts += [(4 + i, 3 + r) for i in range(w)]
    return icon(pts)


def pause16():
    return icon([(x, y) for x in (4, 5, 6, 9, 10, 11) for y in range(3, 13)])


def step16():
    pts = []
    for r in range(10):
        w = min(2 * (min(r, 9 - r) + 1), 8)
        pts += [(2 + i, 3 + r) for i in range(w)]
    pts += [(x, y) for x in (12, 13) for y in range(3, 13)]
    return icon(pts)


def reset16():
    pts = [(x, 3) for x in range(5, 10)] + [(x, 12) for x in range(5, 11)]
    pts += [(3, y) for y in range(5, 11)] + [(12, y) for y in range(7, 11)]
    pts += [(4, 4), (4, 11), (11, 11)]
    pts += [(9, 1), (10, 2), (11, 3), (10, 4), (9, 5), (10, 3)]
    return icon(pts)


SPEAKER = [(x, y) for x in (2, 3, 4) for y in range(6, 10)]
SPEAKER += [(5, y) for y in range(5, 11)] + [(6, y) for y in range(4, 12)] + [(7, y) for y in range(3, 13)]


def sound16():
    pts = SPEAKER + [(9, 6), (10, 7), (10, 8), (9, 9)]
    pts += [(11, 4), (12, 5), (13, 6), (13, 7), (13, 8), (13, 9), (12, 10), (11, 11)]
    return icon(pts)


def mute16():
    pts = SPEAKER + [(10 + i, 6 + i) for i in range(4)] + [(13 - i, 6 + i) for i in range(4)]
    return icon(pts)


def fx16():
    pts = [(6, y) for y in range(2, 13)] + [(x, 7) for x in range(1, 12)]
    pts += [(5, 6), (7, 6), (5, 8), (7, 8)]
    pts += [(12, 2), (11, 3), (12, 3), (13, 3), (12, 4)]
    pts += [(12, 11), (11, 12), (12, 12), (13, 12), (12, 13)]
    return icon(pts, "ask")


def done8():
    s = Sprite(8, 8)
    s.rect(0, 0, 7, 7, "out")
    s.rect(1, 1, 6, 6, "ok")
    s.pts([(2, 4), (3, 5), (4, 4), (5, 3), (6, 2)], "out")
    return s


def todo8():
    s = Sprite(8, 8)
    s.rect(0, 0, 7, 7, "metal-l")
    s.rect(1, 1, 6, 6, "well")
    return s


def caret8():
    s = Sprite(8, 8)
    s.hline(1, 5, 3, "ink")
    s.hline(2, 4, 4, "ink")
    s.px(3, 5, "ink")
    return s


def logo16():
    s = Sprite(16, 16)
    s.rect(1, 2, 13, 14, "acc-d")
    s.rect(2, 1, 14, 13, "acc")
    s.clear([(1, 2), (1, 14), (13, 14), (2, 1), (14, 1), (14, 13)])
    s.px(2, 13, "acc-d")
    s.hline(3, 13, 1, "acc-l")
    s.rect(5, 4, 6, 5, "acc-dd")
    s.rect(10, 8, 11, 9, "acc-dd")
    return s


SPRITES = {
    "host": host32, "switch": switch32, "router": router32,
    "host-small": host16, "switch-small": switch16, "router-small": router16,
    "arp-request": arp_request16, "arp-reply": arp_reply16,
    "echo-request": echo_request16, "echo-reply": echo_reply16,
    "play": play16, "pause": pause16, "step": step16, "reset": reset16,
    "sound": sound16, "mute": mute16, "effects": fx16,
    "goal-done": done8, "goal-open": todo8, "caret": caret8, "logo": logo16,
}


def colour(pal, role):
    if role in ("out", "screen", "ink", "well"):
        return pal[role]
    if role.startswith("metal"):
        return pal[{"metal-dd": "metalDD", "metal-d": "metalD", "metal": "metal", "metal-l": "metalL"}[role]]
    family, _, tone = role.partition("-")
    return pal[family][TONES.index(tone)]


def rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def luminance(h):
    out = []
    for v in rgb(h):
        v /= 255
        out.append(v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4)
    return 0.2126 * out[0] + 0.7152 * out[1] + 0.0722 * out[2]


def contrast(a, b):
    la, lb = sorted((luminance(a), luminance(b)), reverse=True)
    return (la + 0.05) / (lb + 0.05)


def main():
    palettes = json.loads(Path(sys.argv[1]).read_text())
    drawn = {name: draw() for name, draw in SPRITES.items()}
    data = {"roles": ROLES, "chars": CHARS[:len(ROLES)],
            "sprites": {n: {"w": s.w, "h": s.h, "d": s.text()} for n, s in drawn.items()}}
    Path(sys.argv[2]).write_text(json.dumps(data, separators=(",", ":")))

    scale, pad = 4, 12
    row_h = 32 * scale + 2 * pad
    width = sum(s.w * scale + pad for s in drawn.values()) + pad
    sheet = Image.new("RGB", (width, row_h * len(palettes)))
    for r, pal in enumerate(palettes):
        sheet.paste(rgb(pal["canvas"]), (0, r * row_h, width, (r + 1) * row_h))
        x = pad
        for s in drawn.values():
            for yy in range(s.h):
                for xx in range(s.w):
                    v = s.g[yy][xx]
                    if v is None:
                        continue
                    x0, y0 = x + xx * scale, r * row_h + pad + yy * scale
                    sheet.paste(rgb(colour(pal, ROLES[v])), (x0, y0, x0 + scale, y0 + scale))
            x += s.w * scale + pad
    sheet.save(sys.argv[3])

    for pal in palettes:
        dark = pal["dark"]
        pairs = [("ink/panel", pal["ink"], pal["panel"]), ("muted/panel", pal["muted"], pal["panel"]),
                 ("ink/btn", pal["ink"], pal["btn"]), ("ink/well", pal["ink"], pal["well"]),
                 ("muted/well", pal["muted"], pal["well"]), ("out/acc", pal["out"], pal["acc"][2])]
        for fam in ("acc", "ok", "link", "ask", "route"):
            t = pal[fam]
            pairs.append((f"{fam} chip", t[3] if dark else t[0], t[0] if dark else t[3]))
            pairs.append((f"{fam} text/panel", t[2] if dark else t[0], pal["panel"]))
        colours = set()
        for k, v in pal.items():
            if isinstance(v, list):
                colours |= set(v)
            elif isinstance(v, str) and v.startswith("#"):
                colours.add(v)
        low = [f"{n} {contrast(a, b):.1f}" for n, a, b in pairs if contrast(a, b) < 4.5]
        print(f"{pal['name']:12} {len(colours):2} colours; below 4.5:1: {', '.join(low) or 'none'}")
    print(len(drawn), "sprites,", len(ROLES), "roles")


main()
