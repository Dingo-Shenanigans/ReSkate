"""The Animator Studio's loading picture in the MARKET's own branding: its wall (dark concrete, grip-tape grit, blue spray
left and right, halftone, grey flecks, a huge worn word low across it), the reskate. wordmark lightly worn, the clean
blue paint band with "animator studio" on it and paint running off it, and BUILT BY on torn paper. The wall is made from
the same pieces as the MARKET (market_art/make_market_art.py), so it is the same look.

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
import make_market_art as art  # noqa: E402  (its pieces: spray, halftone, grey_dots, grunge_word)

REPO = HERE.parents[1]
FONTS = REPO / "External" / "fonts"
MARKET = REPO / "assets" / "animator_studio" / "market"
OUT_FULL = r"C:\Users\Fourt\Documents\Reskate\Animator Studio backdrop 6.png"
OUT_ASSET = REPO / "assets" / "animator_studio" / "loading.jpg"
PREVIEW = Path(r"C:\Users\Fourt\AppData\Local\Temp\claude\C--Users-Fourt\2ab0674e-9ffe-4afb-a488-7a96406c2044\scratchpad\thumb")

W, H = 3840, 1620           # 21:9, as the MARKET's wall
SEEN = round(H * 16 / 9)    # what a 16:9 screen shows of it: the middle (the game fits it to cover the screen)
L0 = (W - SEEN) // 2        # its left edge
BLUE = art.BLUE
INK = (16, 18, 24)
PAPER = (244, 241, 233)
CREDITS = "22SH, FOURTH, NORMS, STEEZ"


def font(name, size):
    return ImageFont.truetype(str(FONTS / name), size)


def layer():
    return Image.new("RGBA", (W, H), (0, 0, 0, 0))


# ---- the wall, as the MARKET's ------------------------------------------------------------------------------
T = art.T
conc = np.asarray(Image.open(MARKET / "concrete.jpg").convert("L"), np.float32) / 255
grip = np.asarray(Image.open(MARKET / "grip.jpg").convert("L"), np.float32) / 255
c = np.tile(conc, (H // T + 1, W // T + 1))[:H, :W]
g = np.tile(grip, (H // T + 1, W // T + 1))[:H, :W]
v = 0.125 + 0.32 * (c - 0.5) + 0.26 * np.clip(g - 0.12, 0, 1)
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
# Lit across the whole picture, edge to edge (a wide screen shows all of it): only a soft fall-off to the sides and
# corners, no darker panel outside the middle.
light = np.exp(-(((xx - W * 0.5) / (W * 0.60)) ** 2 + ((yy - H * 0.42) / (H * 0.80)) ** 2))
edge = np.clip(1.08 - 0.30 * np.sqrt(((xx - W / 2) / (W * 0.55)) ** 2 + ((yy - H * 0.45) / (H * 0.70)) ** 2), 0.72, 1)
v = v * (0.80 + 0.30 * light) * edge
wall = Image.fromarray(np.clip(np.dstack([v * 255 * 0.84, v * 255 * 0.92, v * 255 * 1.12]), 0, 255).astype(np.uint8)).convert("RGBA")
# The huge worn word low across it, barely there.
ghost = art.grunge_word("reskate.", FONTS / "Montserrat-ExtraBold.ttf", 900, 7, 0.36)
ghost.putalpha(ghost.getchannel("A").point(lambda p: int(p * 0.075)))
wall.alpha_composite(ghost, (int(W * 0.5 - ghost.width / 2), int(H - ghost.height + 120)))
# Halftone: blue from the picture's low right corner, grey from its top left, the dots gone before the edges of their
# own tile (no straight cut through them), and still there in the corners of a 16:9 screen.


def corner_halftone(corner, step, reach, colour, alpha):
    side = int(reach * 0.86) + 2 * step  # the dots are gone by 83% of the reach
    tw, th = min(side, W), min(side, H)
    at = (0 if corner[0] == 0 else W - tw, 0 if corner[1] == 0 else H - th)
    tile = art.halftone(tw, th, step, (corner[0] - at[0], corner[1] - at[1]), reach, colour, alpha)
    edges = np.asarray(tile.getchannel("A"))
    inner = edges[:, -2:] if corner[0] == 0 else edges[:, :2]
    assert th == H or (edges[-2:, :] if corner[1] == 0 else edges[:2, :]).max() == 0, "halftone cut at its tile's edge"
    assert inner.max() == 0, "halftone cut at its tile's edge"
    wall.alpha_composite(tile, at)


corner_halftone((W, H), 22, 1550, BLUE, 105)
corner_halftone((0, 0), 20, 1050, (170, 178, 196), 30)
# Spray: one on the left (high) and one on the right (low), each with a small one near it, all whole on a 16:9 screen.
# The droplets thin out to nothing by REACH x its radius (the market's spray flings the odd one much further), and each
# splatter sits at least that far in from the screen's edges, so none of it is cut off.
REACH = 1.95
view = np.clip(np.minimum((xx - L0) / 40, (L0 + SEEN - xx) / 40), 0, 1) * np.clip(np.minimum(yy / 40, (H - yy) / 40), 0, 1)
sprays = layer()
for cx, cy, r, droplets, seed, runs in ((L0 + 385, 425, 180, 2400, 12, 5), (L0 + 250, 770, 55, 320, 13, 1),
                                        (L0 + SEEN - 405, H - 445, 190, 2600, 11, 5), (L0 + SEEN - 205, H - 770, 60, 360, 14, 1)):
    half = int(r * REACH) + 8
    blot = art.spray(2 * half, 2 * half, seed, [(half, half, r, droplets)], drips=runs)
    by_, bx_ = np.mgrid[0:2 * half, 0:2 * half].astype(np.float32)
    far = np.hypot(bx_ - half, by_ - half) / r
    fade = np.clip((REACH - far) / (REACH - 1.55), 0, 1) ** 1.5
    blot.putalpha(Image.fromarray((np.asarray(blot.getchannel("A"), np.float32) * fade).astype(np.uint8)))
    assert L0 + 20 <= cx - half and cx + half <= L0 + SEEN - 20 and 20 <= cy - half and cy + half <= H - 20, ("spray off the screen", cx, cy, r)
    sprays.alpha_composite(blot, (cx - half, cy - half))
lost = np.asarray(sprays.getchannel("A"), np.float32) * (1 - view)
assert lost.max() == 0, "spray past the screen's edge"
wall.alpha_composite(sprays)
wall.alpha_composite(art.grey_dots(W, H, 14, 420))
canvas = wall

# ---- the sizes: the word three quarters of what a 16:9 screen shows ------------------------------------------
probe = ImageDraw.Draw(Image.new("L", (10, 10)))
WORD = "reskate."
size = 700
while probe.textlength(WORD, font=font("Montserrat-ExtraBold.ttf", size)) > SEEN * 0.74:
    size -= 4
wf = font("Montserrat-ExtraBold.ttf", size)
word_w = probe.textlength(WORD, font=wf)
cap_h = -probe.textbbox((0, 0), WORD, font=wf, anchor="ls")[1]
band_h = size * 0.46
band_gap = size * 0.10       # baseline to the band's top
credits_gap = size * 0.22    # band's bottom to the paper
strip_h = int(size * 0.22)
block_h = cap_h + band_gap + band_h + credits_gap + strip_h
top = H / 2 - block_h / 2 - size * 0.03
base_y = top + cap_h
x0 = W / 2 - word_w / 2

# ---- the word, in one go (the font's own spacing), its edges clean -----------------------------------------------
# Worn only lightly: the ink a touch thinner in soft patches, no speckle or scratches, so it reads clean.
letters = Image.new("L", (W, H), 0)
ImageDraw.Draw(letters).text((x0, base_y), WORD, font=wf, fill=255, anchor="ls")
nrng = np.random.default_rng(7)


def noise(cells_x, cells_y):
    return np.asarray(Image.fromarray(nrng.random((cells_y, cells_x)).astype(np.float32), mode="F").resize((W, H), Image.BICUBIC), np.float32)


patches = 0.55 * noise(W // 260 + 2, H // 260 + 2) + 0.30 * noise(W // 90 + 2, H // 90 + 2) + 0.15 * noise(W // 30 + 2, H // 30 + 2)
thin = 1 - np.clip((patches - 0.30) / 0.30, 0, 1)  # 0 full ink, 1 where it wore thinnest
la = np.asarray(letters, np.float32) / 255
word_rgba = np.zeros((H, W, 4), np.uint8)
word_rgba[..., :3] = np.clip(np.array(PAPER, np.float32)[None, None, :] * (1 - 0.06 * thin)[..., None], 0, 255).astype(np.uint8)
word_rgba[..., 3] = (np.clip(la * (1 - 0.07 * thin), 0, 1) * 255).astype(np.uint8)
shadow = layer()
ImageDraw.Draw(shadow).text((x0 + size * 0.022, base_y + size * 0.03), WORD, font=wf, fill=(0, 0, 0, 160), anchor="ls")
canvas = Image.alpha_composite(canvas, shadow.filter(ImageFilter.GaussianBlur(size * 0.03)))
canvas = Image.alpha_composite(canvas, Image.fromarray(word_rgba))

# ---- the paint: one clean band, its edges soft and a little uneven, its ends rounded; drips running off it ------------
# The same band as make_loading_concrete.py: no dry brush ends, so no lines out at its sides.
rnd = random.Random(11)
SCALE = 2  # drawn at half size, blurred and cut at the middle grey: smooth, organic edges
mask = Image.new("L", (W // SCALE, H // SCALE), 0)
md = ImageDraw.Draw(mask)
t0x, t1x = (x0 + word_w * 0.10) / SCALE, (x0 + word_w * 0.92) / SCALE
t0y = (base_y + band_gap) / SCALE
bh = band_h / SCALE
phase = [rnd.uniform(0, 6.28) for _ in range(10)]


def sway(s, k, amp):
    # the stroke's sway, and the brush's own small unevenness along the edge
    return amp * (0.50 * math.sin(s * 3.1 + phase[k]) + 0.26 * math.sin(s * 9.7 + phase[k + 1]) + 0.14 * math.sin(s * 23.0 + phase[k + 2]) +
                  0.07 * math.sin(s * 71.0 + phase[k + 1] * 3) + 0.05 * math.sin(s * 137.0 + phase[k] * 5))


steps = 400
top_pts, bottom_pts = [], []
for i in range(steps + 1):
    s = i / steps
    x = t0x + (t1x - t0x) * s
    thick = bh * (1.0 - 0.06 * s)  # the brush loads at the start and runs a touch thinner towards the end
    mid = t0y + bh / 2 + sway(s, 6, bh * 0.05)
    top_pts.append((x, mid - thick / 2 + sway(s, 0, bh * 0.07)))
    bottom_pts.append((x, mid + thick / 2 + sway(s, 3, bh * 0.07)))
md.polygon(top_pts + bottom_pts[::-1], fill=255)
# Rounded ends, flatter and a touch lopsided, as a brush leaves them.
for (x, yt), (_, yb), lean in ((top_pts[0], bottom_pts[0], -1), (top_pts[-1], bottom_pts[-1], 1)):
    r = (yb - yt) / 2
    cx, cy = x + lean * r * 0.05, (yt + yb) / 2 + r * 0.06 * lean
    md.ellipse([cx - r * 0.58, cy - r * 0.97, cx + r * 0.58, cy + r * 1.0], fill=255)
    md.ellipse([cx + lean * r * 0.12 - r * 0.34, cy - r * 0.72, cx + lean * r * 0.12 + r * 0.34, cy + r * 0.86], fill=255)
# Drips: a neck where the paint gathers under the band, a thin run down, and a bead at its end.
bottom_at = lambda x: bottom_pts[min(steps, max(0, round((x - t0x) / (t1x - t0x) * steps)))][1]
for at, length, width in ((0.07, 0.55, 0.050), (0.19, 0.22, 0.034), (0.43, 0.36, 0.040), (0.66, 0.18, 0.030), (0.80, 0.70, 0.046), (0.93, 0.28, 0.036)):
    x = t0x + (t1x - t0x) * at
    y0 = bottom_at(x) - bh * 0.08
    run = bh * length * 2.2
    w0 = bh * width * 2.0  # the run's width
    neck = w0 * 4.0        # how wide it gathers at the band
    pts_l, pts_r = [], []
    for j in range(41):
        u = j / 40
        y = y0 + run * u
        wdt = w0 + (neck - w0) * (1 - u) ** 5 + w0 * 0.15 * math.exp(-((u - 0.85) / 0.08) ** 2)
        wobble = w0 * 0.12 * math.sin(u * 7 + at * 20)
        pts_l.append((x - wdt / 2 + wobble, y))
        pts_r.append((x + wdt / 2 + wobble, y))
    md.polygon(pts_l + pts_r[::-1], fill=255)
    bead = w0 * 0.95
    md.ellipse([x - bead, y0 + run - bead * 0.9, x + bead, y0 + run + bead * 1.1], fill=255)
mask = mask.filter(ImageFilter.GaussianBlur(3.2)).point(lambda p: 255 if p > 118 else 0).filter(ImageFilter.GaussianBlur(0.8))
mask = mask.resize((W, H), Image.LANCZOS)
# The paint itself: the blue, a little darker where the brush ran thin, a sheen along the top of the band.
paint_rgb = np.zeros((H, W, 3), dtype=np.float32)
paint_rgb[...] = BLUE
streak = np.asarray(Image.effect_noise((48, H // 3), 40).resize((W, H), Image.BICUBIC)).astype(np.float32)  # along the stroke
paint_rgb *= (0.93 + 0.07 * (streak / 255.0))[..., None]
sheen_y0, sheen_y1 = (t0y + bh * 0.08) * SCALE, (t0y + bh * 0.48) * SCALE
sheen = np.clip(1 - np.abs(yy - (sheen_y0 + sheen_y1) / 2) / ((sheen_y1 - sheen_y0) / 2), 0, 1) ** 2
paint_rgb = np.clip(paint_rgb + sheen[..., None] * 9, 0, 255)
paint = Image.fromarray(paint_rgb.astype(np.uint8)).convert("RGBA")
paint.putalpha(mask)
paint_shadow = layer()
paint_shadow.putalpha(mask.point(lambda p: p * 120 // 255))
shadow_moved = layer()
shadow_moved.paste(paint_shadow, (int(size * 0.012), int(size * 0.02)))
canvas = Image.alpha_composite(canvas, shadow_moved.filter(ImageFilter.GaussianBlur(size * 0.02)))
canvas = Image.alpha_composite(canvas, paint)
# Its words, in the middle of the band.
label = layer()
lb = ImageDraw.Draw(label)
lf = font("Montserrat-ExtraBold.ttf", round(band_h * 0.62))
text_w = lb.textlength("animator studio", font=lf)
lb.text(((t0x + t1x) * SCALE / 2 - text_w / 2, base_y + band_gap + band_h * 0.5), "animator studio", font=lf, fill=INK + (255,), anchor="lm")
canvas = Image.alpha_composite(canvas, label)

# ---- BUILT BY, on a strip of torn white paper, a little crooked ------------------------------------------------------
prnd = random.Random(24)
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
    pts.append((x, M + prnd.uniform(-5, 5)))
    x += prnd.uniform(10, 26)
pts.append((M + pw + prnd.uniform(-6, 6), M + prnd.uniform(-4, 4)))
for t in (0.3, 0.6, 0.9):
    pts.append((M + pw + prnd.uniform(-7, 7), M + ph * t))
x = M + pw
while x > M:  # the bottom edge, torn
    pts.append((x, M + ph + prnd.uniform(-6, 6)))
    x -= prnd.uniform(10, 26)
for t in (0.9, 0.6, 0.3):
    pts.append((M + prnd.uniform(-7, 7), M + ph * t))
ImageDraw.Draw(torn).polygon(pts, fill=255)
tex = paper_tex.resize((max(pw + 2 * M, 1024), max(ph + 2 * M, 1024))).crop((0, 0, pw + 2 * M, ph + 2 * M)).convert("RGBA")
tex.putalpha(torn)
sheet.alpha_composite(tex)
sd = ImageDraw.Draw(sheet)
sd.text((M + pad, M + ph / 2), "BUILT BY ", font=light_f, fill=(70, 70, 76, 255), anchor="lm")
sd.text((M + pad + w1, M + ph / 2), CREDITS, font=bold_f, fill=INK + (255,), anchor="lm")
sheet = sheet.rotate(-1.2, expand=True, resample=Image.BICUBIC)
sh = Image.new("RGBA", sheet.size, (0, 0, 0, 0))
sh.putalpha(sheet.getchannel("A").point(lambda p: int(p * 0.6)))
sy = int(base_y + band_gap + band_h + credits_gap) - (sheet.height - ph) // 2
sx = int(W / 2 - sheet.width / 2)
lay = layer()
lay.alpha_composite(sh, (sx + 10, sy + 18))
canvas = Image.alpha_composite(canvas, lay.filter(ImageFilter.GaussianBlur(12)))
canvas.alpha_composite(sheet, (sx, sy))

# ---- grain over all of it, as the MARKET's wall has (lighter over the word) -------------------------------------------
final = canvas.convert("RGB")
grain = np.asarray(Image.effect_noise((W, H), 20)).astype(np.float32) - 128
on_word = np.asarray(letters.filter(ImageFilter.MaxFilter(5)), np.float32) / 255
final = Image.fromarray(np.clip(np.asarray(final).astype(np.float32) + (grain * 0.14 * (1 - 0.7 * on_word))[..., None], 0, 255).astype(np.uint8))
final.save(OUT_FULL)
final.resize((3072, round(3072 * H / W)), Image.LANCZOS).save(OUT_ASSET, quality=93, subsampling=0, optimize=True)
final.crop((L0, 0, L0 + SEEN, H)).resize((1280, 720), Image.LANCZOS).save(PREVIEW / "loading_gritty_169.jpg", quality=90)
final.crop((int(W * 0.30), int(H * 0.22), int(W * 0.62), int(H * 0.78))).save(PREVIEW / "loading_gritty_zoom.jpg", quality=90)
print("ok", size, OUT_FULL)
