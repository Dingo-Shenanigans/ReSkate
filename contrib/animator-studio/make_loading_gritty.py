"""The Animator Studio's loading picture in the MARKET's own branding: its wall (dark concrete, grip-tape grit, blue spray
in the corners, halftone, grey flecks, a huge worn word low across it), the reskate. wordmark printed and worn, the dry
blue brush stroke with "animator studio" on it and paint running off it, and BUILT BY on torn paper. Made from the same
pieces as the MARKET (market_art/make_market_art.py), so it is the same look.

    python make_loading_gritty.py

Writes "Animator Studio backdrop 6.png" and assets/animator_studio/loading.jpg (what ReSkate.dll is built with).
"""
import math
import random
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "market_art"))
import make_market_art as art  # noqa: E402  (its pieces: spray, halftone, grey_dots, grunge_word, brush_stroke)

REPO = HERE.parents[1]
FONTS = REPO / "External" / "fonts"
MARKET = REPO / "assets" / "animator_studio" / "market"
OUT_FULL = r"C:\Users\Fourt\Documents\Reskate\Animator Studio backdrop 6.png"
OUT_ASSET = REPO / "assets" / "animator_studio" / "loading.jpg"
PREVIEW = Path(r"C:\Users\Fourt\AppData\Local\Temp\claude\C--Users-Fourt\2ab0674e-9ffe-4afb-a488-7a96406c2044\scratchpad\thumb")

W, H = 3840, 1620           # 21:9, as the MARKET's wall
SEEN = round(H * 16 / 9)    # what a 16:9 screen shows of it: the middle
L0 = (W - SEEN) // 2        # its left edge
BLUE = art.BLUE
INK = (16, 18, 24)
CREDITS = "22SH, FOURTH, NORMS, STEEZ"
rnd = random.Random(24)


def font(name, size):
    return ImageFont.truetype(str(FONTS / name), size)


def brush(width, height, seed):
    """The MARKET's dry blue brush stroke (make_market_art.brush_stroke), its dry ends short: no streaks out at its sides."""
    rng = np.random.default_rng(seed)
    yy_, xx_ = np.mgrid[0:height, 0:width].astype(np.float32)
    u, v_ = xx_ / width, (yy_ - height / 2) / (height / 2)
    wob = lambda: np.asarray(Image.fromarray((rng.random((1, 40)) * 255).astype(np.uint8)).resize((width, 1), Image.BICUBIC), np.float32)[0] / 255 - 0.5
    top_, bottom_ = 0.80 + 0.10 * wob(), 0.82 + 0.10 * wob()
    # rounded off at its ends, so it does not end in a square
    end = np.clip(np.minimum(u / 0.035, (1 - u) / 0.035), 0, 1)
    reach = np.sqrt(np.clip(1 - (1 - end) ** 2, 0, 1))
    inside = (v_ > -top_[None, :] * reach) & (v_ < bottom_[None, :] * reach)
    streaks = np.asarray(Image.fromarray((rng.random((height, 8)) * 255).astype(np.uint8)).resize((width, height), Image.BICUBIC), np.float32) / 255
    dry = np.clip(np.minimum(u / 0.05, (1 - u) / 0.06), 0, 1)
    a = inside * np.clip((streaks - (1 - dry) * 0.9) * 3 + dry * 0.75, 0, 1)
    a *= (rng.random((height, width)) > 0.03 * (1 - dry) + 0.008)
    out = np.zeros((height, width, 4), np.uint8)
    out[..., 0], out[..., 1], out[..., 2] = art.BLUE
    out[..., 3] = (np.clip(a, 0, 1) * 255).astype(np.uint8)
    return Image.fromarray(out)


# ---- the wall, as the MARKET's ------------------------------------------------------------------------------
T = art.T
conc = np.asarray(Image.open(MARKET / "concrete.jpg").convert("L"), np.float32) / 255
grip = np.asarray(Image.open(MARKET / "grip.jpg").convert("L"), np.float32) / 255
c = np.tile(conc, (H // T + 1, W // T + 1))[:H, :W]
g = np.tile(grip, (H // T + 1, W // T + 1))[:H, :W]
v = 0.125 + 0.32 * (c - 0.5) + 0.26 * np.clip(g - 0.12, 0, 1)
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
light = np.exp(-(((xx - W * 0.5) / (W * 0.42)) ** 2 + ((yy - H * 0.42) / (H * 0.75)) ** 2))
edge = np.clip(1.15 - 0.55 * np.sqrt(((xx - W / 2) / (SEEN * 0.62)) ** 2 + ((yy - H * 0.45) / (H * 0.65)) ** 2), 0.42, 1)
v = v * (0.72 + 0.42 * light) * edge
wall = Image.fromarray(np.clip(np.dstack([v * 255 * 0.84, v * 255 * 0.92, v * 255 * 1.12]), 0, 255).astype(np.uint8)).convert("RGBA")
# The huge worn word low across it, barely there.
ghost = art.grunge_word("reskate.", FONTS / "Montserrat-ExtraBold.ttf", 900, 7, 0.36)
ghost.putalpha(ghost.getchannel("A").point(lambda p: int(p * 0.075)))
wall.alpha_composite(ghost, (int(W * 0.5 - ghost.width / 2), int(H - ghost.height + 120)))
# Halftone: blue in the low right, grey in the top left; spray in those corners and one low down; grey flecks.
wall.alpha_composite(art.halftone(1400, 1000, 22, (1400, 1000), 1150, BLUE, 105), (L0 + SEEN - 1400 + 120, H - 1000))
wall.alpha_composite(art.halftone(900, 700, 20, (0, 0), 800, (170, 178, 196), 30), (L0 - 120, 0))
wall.alpha_composite(art.spray(1400, 1000, 11, [(1120, 760, 300, 3800), (520, 960, 100, 600)], drips=7), (L0 + SEEN - 1400 + 160, H - 1000 + 40))
wall.alpha_composite(art.spray(1100, 900, 12, [(220, 220, 270, 3200)], drips=5), (L0 - 260, -120))
wall.alpha_composite(art.spray(600, 480, 13, [(300, 230, 95, 700)], drips=3), (L0 + int(SEEN * 0.14), H - 470))
wall.alpha_composite(art.grey_dots(W, H, 14, 420))
canvas = wall

# ---- the lockup, big in the middle ----------------------------------------------------------------------------------
WORD_W = SEEN * 0.74
probe = ImageDraw.Draw(Image.new("L", (10, 10)))
size = 700
while probe.textlength("reskate.", font=font("Montserrat-ExtraBold.ttf", size)) > WORD_W:
    size -= 4
word = art.grunge_word("reskate.", FONTS / "Montserrat-ExtraBold.ttf", size, 3, 0.10)  # printed, worn
# Still whole: the worn print over the solid letters (86% where it flaked), so it reads at a glance. The solid letters are
# set where grunge_word sets them (30 px in from its box).
wf_ = font("Montserrat-ExtraBold.ttf", size)
box_ = probe.textbbox((0, 0), "reskate.", font=wf_)
clean = Image.new("L", word.size, 0)
ImageDraw.Draw(clean).text((30 - box_[0], 30 - box_[1]), "reskate.", font=wf_, fill=255)
solid = np.asarray(clean, np.float32)
worn = np.asarray(word.getchannel("A"), np.float32)
word.putalpha(Image.fromarray(np.maximum(worn, solid * 0.86).astype(np.uint8)))
# grunge_word leaves 30 px round the letters; its box is the letters' own.
band_w, band_h = int(word.width * 0.86), int(size * 0.42)
gap1, gap2 = int(size * 0.06), int(size * 0.30)
strip_h = int(size * 0.22)
block_h = word.height - 60 + gap1 + band_h + gap2 + strip_h
top = int(H / 2 - block_h / 2) - 30
wx = int(W / 2 - word.width / 2)
shadow = Image.new("RGBA", word.size, (0, 0, 0, 0))
shadow.putalpha(word.getchannel("A").point(lambda p: int(p * 0.55)))
shadow_layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
shadow_layer.alpha_composite(shadow, (wx + int(size * 0.018), top + int(size * 0.03)))
canvas = Image.alpha_composite(canvas, shadow_layer.filter(ImageFilter.GaussianBlur(size * 0.025)))
paper_white = (244, 241, 233)
word_rgb = Image.new("RGBA", word.size, paper_white + (255,))
word_rgb.putalpha(word.getchannel("A"))
canvas.alpha_composite(word_rgb, (wx, top))
word_bottom = top + word.height - 30

# The blue: the MARKET's dry brush stroke, laid under the word.
bx, by = int(W / 2 - band_w / 2), word_bottom + gap1
stroke = brush(band_w, band_h, 41)
# Drips running off its bottom: gather under it, run thin, end in a bead. Drawn at twice the size, blurred and cut.
S = 2
dm = Image.new("L", (band_w * S, (band_h + int(size * 0.9)) * S), 0)
dd = ImageDraw.Draw(dm)
for at, length, width in ((0.10, 0.75, 0.050), (0.24, 0.30, 0.034), (0.47, 0.48, 0.040), (0.69, 0.24, 0.030), (0.82, 0.95, 0.046), (0.94, 0.36, 0.036)):
    x = band_w * at
    y0 = band_h * 0.80
    run = band_h * length * 2.0
    w0 = band_h * width * 1.6
    neck = w0 * 4.0
    left, right = [], []
    for j in range(41):
        u = j / 40
        y = y0 + run * u
        wdt = w0 + (neck - w0) * (1 - u) ** 5 + w0 * 0.15 * math.exp(-((u - 0.85) / 0.08) ** 2)
        wob = w0 * 0.12 * math.sin(u * 7 + at * 20)
        left.append(((x - wdt / 2 + wob) * S, y * S))
        right.append(((x + wdt / 2 + wob) * S, y * S))
    dd.polygon(left + right[::-1], fill=255)
    bead = w0 * 0.95
    dd.ellipse([(x - bead) * S, (y0 + run - bead * 0.9) * S, (x + bead) * S, (y0 + run + bead * 1.1) * S], fill=255)
dm = dm.filter(ImageFilter.GaussianBlur(3)).point(lambda p: 255 if p > 118 else 0).resize((band_w, band_h + int(size * 0.9)), Image.LANCZOS)
drips = Image.new("RGBA", dm.size, BLUE + (0,))
drips.putalpha(dm)
paint = Image.new("RGBA", dm.size, (0, 0, 0, 0))
paint.alpha_composite(drips)
paint.alpha_composite(stroke, (0, 0))
paint_shadow = Image.new("RGBA", paint.size, (0, 0, 0, 0))
paint_shadow.putalpha(paint.getchannel("A").point(lambda p: int(p * 0.45)))
layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
layer.alpha_composite(paint_shadow, (bx + 8, by + 14))
canvas = Image.alpha_composite(canvas, layer.filter(ImageFilter.GaussianBlur(10)))
canvas.alpha_composite(paint, (bx, by))
# Its words, printed in ink, worn a little.
label = art.grunge_word("animator studio", FONTS / "Montserrat-ExtraBold.ttf", int(band_h * 0.56), 5, 0.06)
label_rgb = Image.new("RGBA", label.size, INK + (255,))
label_rgb.putalpha(label.getchannel("A"))
canvas.alpha_composite(label_rgb, (int(W / 2 - label.width / 2), int(by + band_h * 0.5 - label.height / 2)))

# ---- BUILT BY, on a strip of torn white paper, a little crooked ------------------------------------------------------
paper_tex = Image.open(MARKET / "paper_white.jpg").convert("RGB")
cs = int(strip_h * 0.48)
light_f, bold_f = font("Montserrat-SemiBold.ttf", cs), font("Montserrat-ExtraBold.ttf", cs)
w1 = probe.textlength("BUILT BY ", font=light_f)
w2 = probe.textlength(CREDITS, font=bold_f)
pad = int(strip_h * 0.55)
pw, ph = int(w1 + w2 + 2 * pad), strip_h
M = 60
sheet = Image.new("RGBA", (pw + 2 * M, ph + 2 * M), (0, 0, 0, 0))
torn = Image.new("L", sheet.size, 0)
pts = []
x = M
while x < M + pw:  # the top edge, torn
    pts.append((x, M + rnd.uniform(-5, 5)))
    x += rnd.uniform(10, 26)
pts.append((M + pw + rnd.uniform(-6, 6), M + rnd.uniform(-4, 4)))
for t in (0.3, 0.6, 0.9):
    pts.append((M + pw + rnd.uniform(-7, 7), M + ph * t))
x = M + pw
while x > M:  # the bottom edge, torn
    pts.append((x, M + ph + rnd.uniform(-6, 6)))
    x -= rnd.uniform(10, 26)
for t in (0.9, 0.6, 0.3):
    pts.append((M + rnd.uniform(-7, 7), M + ph * t))
ImageDraw.Draw(torn).polygon(pts, fill=255)
tex = paper_tex.resize((max(pw + 2 * M, 1024), max(ph + 2 * M, 1024))).crop((0, 0, pw + 2 * M, ph + 2 * M)).convert("RGBA")
tex.putalpha(torn)
sheet.alpha_composite(tex)
sd = ImageDraw.Draw(sheet)
sd.text((M + pad, M + ph / 2), "BUILT BY ", font=light_f, fill=(70, 70, 76, 255), anchor="lm")
sd.text((M + pad + w1, M + ph / 2), CREDITS, font=bold_f, fill=INK + (255,), anchor="lm")
paper_only = sheet.copy()
# Two strips of tape over its ends, their ends torn, a sheen along them.
for tx, ang in ((M - 20, 30), (M + pw - 150, -26)):
    tw, th = 180, 58
    tape = Image.new("RGBA", (tw, th), (0, 0, 0, 0))
    edge_pts = [(x, rnd.uniform(0, 2)) for x in range(0, tw + 1, 18)]
    edge_pts += [(tw - rnd.uniform(0, 12), y) for y in range(0, th + 1, 7)]
    edge_pts += [(x, th - rnd.uniform(0, 2)) for x in range(tw, -1, -18)]
    edge_pts += [(rnd.uniform(0, 12), y) for y in range(th, -1, -7)]
    ImageDraw.Draw(tape).polygon(edge_pts, fill=(222, 222, 216, 200))
    ImageDraw.Draw(tape).rectangle([0, th * 0.22, tw, th * 0.40], fill=(255, 255, 255, 30))
    tape = tape.rotate(ang, expand=True, resample=Image.BICUBIC)
    sheet.alpha_composite(tape, (int(tx), int(M - tape.height / 2 + 6)))
sheet = sheet.rotate(-1.2, expand=True, resample=Image.BICUBIC)
paper_only = paper_only.rotate(-1.2, expand=True, resample=Image.BICUBIC)
sh = Image.new("RGBA", sheet.size, (0, 0, 0, 0))
sh.putalpha(paper_only.getchannel("A").point(lambda p: int(p * 0.6)))
sy = by + band_h + gap2 - M
sx = int(W / 2 - sheet.width / 2)
layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
layer.alpha_composite(sh, (sx + 10, sy + 18))
canvas = Image.alpha_composite(canvas, layer.filter(ImageFilter.GaussianBlur(12)))
canvas.alpha_composite(sheet, (sx, sy))

# ---- grain over all of it, as the MARKET's wall has ---------------------------------------------------------------
final = canvas.convert("RGB")
noise = np.asarray(Image.effect_noise((W, H), 20)).astype(np.float32) - 128
final = Image.fromarray(np.clip(np.asarray(final).astype(np.float32) + noise[..., None] * 0.14, 0, 255).astype(np.uint8))
final.save(OUT_FULL)
final.resize((3072, round(3072 * H / W)), Image.LANCZOS).save(OUT_ASSET, quality=93, subsampling=0, optimize=True)
final.crop((L0, 0, L0 + SEEN, H)).resize((1280, 720), Image.LANCZOS).save(PREVIEW / "loading_gritty_169.jpg", quality=90)
final.crop((int(W * 0.30), int(H * 0.22), int(W * 0.62), int(H * 0.78))).save(PREVIEW / "loading_gritty_zoom.jpg", quality=90)
print("ok", size, OUT_FULL)
