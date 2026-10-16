"""Draws ReSkate's own tab icons (for the custom item tabs) in the game's style: flat white silhouettes
on a transparent ground, one per cell. The game's own category icons (tops, shoes, headwear...) are
read from the installed game at start-up; these are the kinds it has none for.

    python make_tab_icons.py      -> tab_icons.png + tab_icons.json beside this file
"""
import json
import math
import os

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
CELL = 96          # pixels of one icon in the pack
SS = 4             # drawn this much larger, then scaled down: smooth edges
S = CELL * SS
W = (255, 255, 255, 255)
CLEAR = (0, 0, 0, 0)


def canvas():
    return Image.new("RGBA", (S, S), CLEAR)


def p(x, y):
    """A point in 0..1 icon space."""
    return (x * S, y * S)


def ring_shape(d, cx, cy, r, w):
    d.ellipse([p(cx - r, cy - r), p(cx + r, cy + r)], outline=W, width=int(w * S))


def ring():
    im = canvas(); d = ImageDraw.Draw(im)
    ring_shape(d, 0.5, 0.62, 0.27, 0.085)
    d.polygon([p(0.5, 0.12), p(0.66, 0.26), p(0.5, 0.40), p(0.34, 0.26)], fill=W)   # the stone
    return im


def earring():
    im = canvas(); d = ImageDraw.Draw(im)
    d.ellipse([p(0.44, 0.08), p(0.56, 0.20)], fill=W)
    ring_shape(d, 0.5, 0.56, 0.30, 0.08)
    return im


def piercing():
    im = canvas(); d = ImageDraw.Draw(im)
    d.ellipse([p(0.10, 0.36), p(0.36, 0.62)], fill=W)
    d.ellipse([p(0.64, 0.36), p(0.90, 0.62)], fill=W)
    d.rectangle([p(0.28, 0.45), p(0.72, 0.53)], fill=W)
    return im


def necklace():
    im = canvas(); d = ImageDraw.Draw(im)
    d.arc([p(0.12, -0.10), p(0.88, 0.70)], 20, 160, fill=W, width=int(0.07 * S))
    d.polygon([p(0.5, 0.62), p(0.64, 0.76), p(0.5, 0.94), p(0.36, 0.76)], fill=W)
    return im


def chain():
    im = canvas(); d = ImageDraw.Draw(im)
    for i in range(4):
        cx = 0.17 + i * 0.22
        if i % 2 == 0:
            d.ellipse([p(cx - 0.14, 0.38), p(cx + 0.14, 0.62)], outline=W, width=int(0.06 * S))
        else:
            d.ellipse([p(cx - 0.14, 0.44), p(cx + 0.14, 0.56)], outline=W, width=int(0.06 * S))
    return im


def scarf():
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([p(0.14, 0.14), p(0.86, 0.36)], radius=int(0.1 * S), fill=W)
    d.polygon([p(0.52, 0.30), p(0.74, 0.30), p(0.70, 0.86), p(0.48, 0.86)], fill=W)
    for i in range(3):
        x = 0.50 + i * 0.08
        d.rectangle([p(x, 0.86), p(x + 0.04, 0.94)], fill=W)
    return im


def bracelet():
    im = canvas(); d = ImageDraw.Draw(im)
    d.ellipse([p(0.12, 0.30), p(0.88, 0.74)], outline=W, width=int(0.09 * S))
    for i in range(5):
        a = math.pi * (0.15 + 0.175 * i)
        x, y = 0.5 + 0.38 * math.cos(a), 0.52 + 0.22 * math.sin(a)
        d.ellipse([p(x - 0.06, y - 0.06), p(x + 0.06, y + 0.06)], fill=W)
    return im


def watch():
    im = canvas(); d = ImageDraw.Draw(im)
    d.rectangle([p(0.36, 0.04), p(0.64, 0.96)], fill=W)
    d.ellipse([p(0.22, 0.26), p(0.78, 0.82)], fill=W)
    d.ellipse([p(0.30, 0.34), p(0.70, 0.74)], fill=CLEAR)
    d.line([p(0.5, 0.54), p(0.5, 0.40)], fill=W, width=int(0.05 * S))
    d.line([p(0.5, 0.54), p(0.60, 0.58)], fill=W, width=int(0.05 * S))
    return im


def gem():
    im = canvas(); d = ImageDraw.Draw(im)
    d.polygon([p(0.24, 0.20), p(0.76, 0.20), p(0.94, 0.40), p(0.5, 0.92), p(0.06, 0.40)], fill=W)
    d.line([p(0.06, 0.40), p(0.94, 0.40)], fill=CLEAR, width=int(0.04 * S))
    d.line([p(0.36, 0.40), p(0.5, 0.88)], fill=CLEAR, width=int(0.03 * S))
    d.line([p(0.64, 0.40), p(0.5, 0.88)], fill=CLEAR, width=int(0.03 * S))
    return im


def crown():
    im = canvas(); d = ImageDraw.Draw(im)
    d.polygon([p(0.10, 0.80), p(0.10, 0.30), p(0.32, 0.52), p(0.5, 0.18), p(0.68, 0.52), p(0.90, 0.30), p(0.90, 0.80)], fill=W)
    for x, y in ((0.10, 0.26), (0.5, 0.14), (0.90, 0.26)):
        d.ellipse([p(x - 0.06, y - 0.06), p(x + 0.06, y + 0.06)], fill=W)
    return im


def star():
    im = canvas(); d = ImageDraw.Draw(im)
    pts = []
    for i in range(10):
        a = -math.pi / 2 + i * math.pi / 5
        r = 0.44 if i % 2 == 0 else 0.19
        pts.append(p(0.5 + r * math.cos(a), 0.54 + r * math.sin(a)))
    d.polygon(pts, fill=W)
    return im


def heart():
    im = canvas(); d = ImageDraw.Draw(im)
    d.ellipse([p(0.08, 0.16), p(0.52, 0.58)], fill=W)
    d.ellipse([p(0.48, 0.16), p(0.92, 0.58)], fill=W)
    d.polygon([p(0.11, 0.46), p(0.89, 0.46), p(0.5, 0.88)], fill=W)
    return im


def skull():
    im = canvas(); d = ImageDraw.Draw(im)
    d.ellipse([p(0.16, 0.08), p(0.84, 0.70)], fill=W)
    d.rectangle([p(0.30, 0.56), p(0.70, 0.88)], fill=W)
    d.ellipse([p(0.28, 0.36), p(0.46, 0.54)], fill=CLEAR)
    d.ellipse([p(0.54, 0.36), p(0.72, 0.54)], fill=CLEAR)
    d.polygon([p(0.5, 0.56), p(0.45, 0.66), p(0.55, 0.66)], fill=CLEAR)
    for x in (0.40, 0.5, 0.60):
        d.rectangle([p(x - 0.015, 0.76), p(x + 0.015, 0.88)], fill=CLEAR)
    return im


def flame():
    im = canvas(); d = ImageDraw.Draw(im)
    d.polygon([p(0.5, 0.04), p(0.70, 0.34), p(0.66, 0.20), p(0.86, 0.52), p(0.80, 0.80), p(0.5, 0.96),
               p(0.20, 0.80), p(0.14, 0.52), p(0.30, 0.30), p(0.34, 0.46)], fill=W)
    d.polygon([p(0.5, 0.50), p(0.64, 0.72), p(0.5, 0.88), p(0.36, 0.72)], fill=CLEAR)
    return im


def bolt():
    im = canvas(); d = ImageDraw.Draw(im)
    d.polygon([p(0.58, 0.04), p(0.20, 0.56), p(0.46, 0.56), p(0.38, 0.96), p(0.80, 0.40), p(0.54, 0.40), p(0.66, 0.04)], fill=W)
    return im


def dice():
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([p(0.12, 0.12), p(0.88, 0.88)], radius=int(0.14 * S), fill=W)
    for x, y in ((0.32, 0.32), (0.68, 0.32), (0.5, 0.5), (0.32, 0.68), (0.68, 0.68)):
        d.ellipse([p(x - 0.075, y - 0.075), p(x + 0.075, y + 0.075)], fill=CLEAR)
    return im


def music():
    im = canvas(); d = ImageDraw.Draw(im)
    d.ellipse([p(0.12, 0.66), p(0.40, 0.90)], fill=W)
    d.ellipse([p(0.58, 0.58), p(0.86, 0.82)], fill=W)
    d.rectangle([p(0.33, 0.14), p(0.40, 0.78)], fill=W)
    d.rectangle([p(0.79, 0.08), p(0.86, 0.70)], fill=W)
    d.polygon([p(0.33, 0.14), p(0.86, 0.06), p(0.86, 0.22), p(0.33, 0.30)], fill=W)
    return im


def flower():
    im = canvas(); d = ImageDraw.Draw(im)
    for i in range(6):
        a = i * math.pi / 3
        x, y = 0.5 + 0.24 * math.cos(a), 0.5 + 0.24 * math.sin(a)
        d.ellipse([p(x - 0.17, y - 0.17), p(x + 0.17, y + 0.17)], fill=W)
    d.ellipse([p(0.38, 0.38), p(0.62, 0.62)], fill=CLEAR)
    d.ellipse([p(0.42, 0.42), p(0.58, 0.58)], fill=W)
    return im


def moon():
    im = canvas(); d = ImageDraw.Draw(im)
    d.ellipse([p(0.10, 0.10), p(0.90, 0.90)], fill=W)
    d.ellipse([p(0.32, 0.02), p(1.02, 0.72)], fill=CLEAR)
    return im


def bone():
    im = canvas(); d = ImageDraw.Draw(im)
    d.line([p(0.26, 0.74), p(0.74, 0.26)], fill=W, width=int(0.16 * S))
    for x, y in ((0.16, 0.72), (0.28, 0.84), (0.72, 0.16), (0.84, 0.28)):
        d.ellipse([p(x - 0.11, y - 0.11), p(x + 0.11, y + 0.11)], fill=W)
    return im


def eye():
    im = canvas(); d = ImageDraw.Draw(im)
    d.chord([p(0.04, 0.20), p(0.96, 0.92)], 180, 360, fill=W)
    d.chord([p(0.04, 0.08), p(0.96, 0.80)], 0, 180, fill=W)
    d.ellipse([p(0.32, 0.32), p(0.68, 0.68)], fill=CLEAR)
    d.ellipse([p(0.40, 0.40), p(0.60, 0.60)], fill=W)
    return im


def bag():
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([p(0.16, 0.34), p(0.84, 0.92)], radius=int(0.10 * S), fill=W)
    d.arc([p(0.32, 0.10), p(0.68, 0.52)], 180, 360, fill=W, width=int(0.07 * S))
    d.rectangle([p(0.30, 0.52), p(0.70, 0.58)], fill=CLEAR)
    return im


def ghost():
    im = canvas(); d = ImageDraw.Draw(im)
    d.ellipse([p(0.18, 0.06), p(0.82, 0.66)], fill=W)
    d.rectangle([p(0.18, 0.36), p(0.82, 0.82)], fill=W)
    for i in range(3):
        x = 0.18 + i * 0.2133
        d.polygon([p(x, 0.82), p(x + 0.2133, 0.82), p(x + 0.1066, 0.96)], fill=W)
    d.ellipse([p(0.34, 0.30), p(0.46, 0.44)], fill=CLEAR)
    d.ellipse([p(0.54, 0.30), p(0.66, 0.44)], fill=CLEAR)
    return im


def spray():
    im = canvas(); d = ImageDraw.Draw(im)
    d.rounded_rectangle([p(0.28, 0.32), p(0.66, 0.96)], radius=int(0.06 * S), fill=W)
    d.rectangle([p(0.36, 0.18), p(0.58, 0.30)], fill=W)
    d.rectangle([p(0.42, 0.10), p(0.52, 0.18)], fill=W)
    for x, y in ((0.64, 0.10), (0.76, 0.06), (0.76, 0.18), (0.88, 0.12), (0.88, 0.24)):
        d.ellipse([p(x - 0.03, y - 0.03), p(x + 0.03, y + 0.03)], fill=W)
    d.rectangle([p(0.28, 0.50), p(0.66, 0.56)], fill=CLEAR)
    return im


def tag():
    im = canvas(); d = ImageDraw.Draw(im)
    d.polygon([p(0.08, 0.40), p(0.40, 0.10), p(0.92, 0.10), p(0.92, 0.70), p(0.40, 0.70)], fill=W)
    d.ellipse([p(0.30, 0.32), p(0.44, 0.46)], fill=CLEAR)
    return im


def folder():
    im = canvas(); d = ImageDraw.Draw(im)
    d.polygon([p(0.08, 0.20), p(0.40, 0.20), p(0.48, 0.30), p(0.92, 0.30), p(0.92, 0.84), p(0.08, 0.84)], fill=W)
    d.line([p(0.08, 0.40), p(0.92, 0.40)], fill=CLEAR, width=int(0.04 * S))
    return im


ICONS = [("ring", ring), ("earring", earring), ("piercing", piercing), ("necklace", necklace), ("chain", chain),
         ("scarf", scarf), ("bracelet", bracelet), ("watch", watch), ("gem", gem), ("crown", crown), ("star", star),
         ("heart", heart), ("skull", skull), ("flame", flame), ("bolt", bolt), ("dice", dice), ("music", music),
         ("flower", flower), ("moon", moon), ("bone", bone), ("eye", eye), ("bag", bag), ("ghost", ghost), ("spray", spray),
         ("tag", tag), ("folder", folder)]


def main():
    columns = 8
    index = {}
    entries = [(name, lambda draw=draw: draw().resize((CELL, CELL), Image.LANCZOS)) for name, draw in ICONS]
    rows = (len(entries) + columns - 1) // columns
    sheet = Image.new("RGBA", (columns * CELL, rows * CELL), CLEAR)
    for i, (name, make) in enumerate(entries):
        icon = make()
        x, y = (i % columns) * CELL, (i // columns) * CELL
        sheet.alpha_composite(icon, (x, y))
        index[name] = {"pos": [x, y], "size": [CELL, CELL]}
    sheet.save(os.path.join(HERE, "tab_icons.png"))
    with open(os.path.join(HERE, "tab_icons.json"), "w") as f:
        json.dump({"name": "ReSkate tab icons", "icons": index}, f, indent=1)
    print(f"{len(ICONS)} icons -> tab_icons.png ({sheet.size[0]}x{sheet.size[1]})")


if __name__ == "__main__":
    main()
