"""The Animator Studio's loading picture, concrete only: no skater. The loading plate's clean left half (the wall and the
floor, warm towards the middle) with its mirror beside it, lit from the middle, and the reskate. lockup big in the middle
of the screen: the wordmark, a clean band of blue paint with "animator studio" on it and real drips running off it, and
BUILT BY. The plate is made by make_loading_plate.py.

    python make_loading_concrete.py

Writes "Animator Studio backdrop 5.png" (full size) and assets/animator_studio/loading.jpg (what ReSkate.dll is built with).
"""
import math
import random
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
PLATE = r"C:\Users\Fourt\AppData\Local\Temp\claude\C--Users-Fourt\2ab0674e-9ffe-4afb-a488-7a96406c2044\scratchpad\backdrop\plate.png"
FONTS = HERE.parents[1] / "External" / "fonts"
OUT_FULL = r"C:\Users\Fourt\Documents\Reskate\Animator Studio backdrop 5.png"
OUT_ASSET = HERE.parents[1] / "assets" / "animator_studio" / "loading.jpg"
PREVIEW = Path(r"C:\Users\Fourt\AppData\Local\Temp\claude\C--Users-Fourt\2ab0674e-9ffe-4afb-a488-7a96406c2044\scratchpad\thumb")

rnd = random.Random(11)
BLUE = (1, 131, 255)
PAPER = (244, 241, 233)
INK = (16, 18, 24)
CREDITS = "22SH, FOURTH, NORMS, STEEZ"


def font(name, size):
    return ImageFont.truetype(str(FONTS / name), size)


plate = Image.open(PLATE).convert("RGB")
W, H = plate.size
half = plate.crop((0, 0, W // 2, H))
base = Image.new("RGB", (W, H))
base.paste(half, (0, 0))
base.paste(half.transpose(Image.FLIP_LEFT_RIGHT), (W // 2, 0))
SEEN = round(H * 16 / 9)  # how much of the width a 16:9 screen shows (the sides are cut off)


def layer():
    return Image.new("RGBA", (W, H), (0, 0, 0, 0))


# ---- grade: cool in the shadows, lit from the middle, deep at the edges ----------------------------------
arr = np.asarray(base).astype(np.float32)
luma = arr.mean(axis=2, keepdims=True) / 255.0
arr = arr * (np.array([0.96, 0.99, 1.06], dtype=np.float32) * (1 - luma) + luma)
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
d = np.sqrt(((xx - W * 0.5) / (W * 0.46)) ** 2 + ((yy - H * 0.48) / (H * 0.78)) ** 2)
arr *= np.clip(1.16 - 0.66 * d ** 1.5, 0.16, 1.1)[..., None]
arr *= np.clip(1.0 - 0.35 * (np.abs(yy / H - 0.47) / 0.53) ** 3, 0.6, 1.0)[..., None]
canvas = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8)).convert("RGBA")
leak = layer()
ImageDraw.Draw(leak).ellipse([W * 0.5 - 2100, H * 0.16 - 900, W * 0.5 + 2100, H * 0.16 + 1100], fill=(1, 110, 255, 30))
canvas = Image.alpha_composite(canvas, leak.filter(ImageFilter.GaussianBlur(420)))

# ---- the sizes: the word three quarters of what a 16:9 screen shows ------------------------------------------
probe = ImageDraw.Draw(layer())
word = "reskate."
WORD_W = SEEN * 0.75
size = 900
while True:
    wf = font("Montserrat-ExtraBold.ttf", size)
    if probe.textlength(word, font=wf) <= WORD_W:
        break
    size -= 4
word_w = probe.textlength(word, font=wf)
ascent, _ = wf.getmetrics()
ink = probe.textbbox((0, 0), word, font=wf, anchor="ls")  # the letters' own box from the baseline
cap_h = -ink[1]
band_h = size * 0.46
band_gap = size * 0.10       # baseline to the band's top
credits_gap = size * 0.20    # band's bottom to the strip
strip_h = size * 0.22
block_h = cap_h + band_gap + band_h + credits_gap + strip_h
top = H / 2 - block_h / 2 - size * 0.03
base_y = top + cap_h
x0 = W / 2 - word_w / 2

# ---- the word, in one go: the font's own spacing (and kerning), so no letter runs into the next ---------------------
type_layer = layer()
ImageDraw.Draw(type_layer).text((x0, base_y), word, font=wf, fill=PAPER + (255,), anchor="ls")
shadow = layer()
ImageDraw.Draw(shadow).text((x0 + size * 0.022, base_y + size * 0.03), word, font=wf, fill=(0, 0, 0, 160), anchor="ls")
canvas = Image.alpha_composite(canvas, shadow.filter(ImageFilter.GaussianBlur(size * 0.03)))
canvas = Image.alpha_composite(canvas, type_layer)

# ---- the paint: one clean band, its edges soft and a little uneven, its ends rounded; drips running off it ------------
SCALE = 2  # drawn at half size, blurred and cut at the middle grey: smooth, organic edges
mw, mh = W // SCALE, H // SCALE
mask = Image.new("L", (mw, mh), 0)
md = ImageDraw.Draw(mask)
t0x, t1x = (x0 + word_w * 0.10) / SCALE, (x0 + word_w * 0.92) / SCALE
t0y = (base_y + band_gap) / SCALE
bh = band_h / SCALE
phase = [rnd.uniform(0, 6.28) for _ in range(10)]


def edge(s, k, amp):
    # the stroke's sway, and the brush's own small unevenness along the edge
    return amp * (0.50 * math.sin(s * 3.1 + phase[k]) + 0.26 * math.sin(s * 9.7 + phase[k + 1]) + 0.14 * math.sin(s * 23.0 + phase[k + 2]) +
                  0.07 * math.sin(s * 71.0 + phase[k + 1] * 3) + 0.05 * math.sin(s * 137.0 + phase[k] * 5))


steps = 400
top_pts, bottom_pts = [], []
for i in range(steps + 1):
    s = i / steps
    x = t0x + (t1x - t0x) * s
    # the brush loads at the start and runs a touch thinner towards the end
    thick = bh * (1.0 - 0.06 * s)
    mid = t0y + bh / 2 + edge(s, 6, bh * 0.05)
    top_pts.append((x, mid - thick / 2 + edge(s, 0, bh * 0.07)))
    bottom_pts.append((x, mid + thick / 2 + edge(s, 3, bh * 0.07)))
md.polygon(top_pts + bottom_pts[::-1], fill=255)
# Rounded ends: where the brush came down and where it lifted.
for (x, yt), (_, yb), lean in ((top_pts[0], bottom_pts[0], -1), (top_pts[-1], bottom_pts[-1], 1)):
    # Not a perfect round: flatter, a touch lopsided, as a brush leaves it.
    r = (yb - yt) / 2
    cx, cy = x + lean * r * 0.05, (yt + yb) / 2 + r * 0.06 * lean
    md.ellipse([cx - r * 0.58, cy - r * 0.97, cx + r * 0.58, cy + r * 1.0], fill=255)
    md.ellipse([cx + lean * r * 0.12 - r * 0.34, cy - r * 0.72, cx + lean * r * 0.12 + r * 0.34, cy + r * 0.86], fill=255)

# Drips: a neck where the paint gathers under the band, a thin run down, and a bead at its end.
bottom_at = lambda x: bottom_pts[min(steps, max(0, round((x - t0x) / (t1x - t0x) * steps)))][1]
drips = [(0.07, 0.55, 0.050), (0.19, 0.22, 0.034), (0.43, 0.36, 0.040), (0.66, 0.18, 0.030), (0.80, 0.70, 0.046), (0.93, 0.28, 0.036)]
for at, length, width in drips:
    x = t0x + (t1x - t0x) * at
    y0 = bottom_at(x) - bh * 0.08
    run = bh * length * 2.2
    w0 = bh * width * 2.0  # the run's width
    neck = w0 * 4.0       # how wide it gathers at the band
    pts_l, pts_r = [], []
    for j in range(41):
        u = j / 40
        y = y0 + run * u
        # wide at the band, narrowing fast into the run, then a slight swell just before the bead
        wdt = w0 + (neck - w0) * (1 - u) ** 5 + w0 * 0.15 * math.exp(-((u - 0.85) / 0.08) ** 2)
        wobble = w0 * 0.12 * math.sin(u * 7 + at * 20)
        pts_l.append((x - wdt / 2 + wobble, y))
        pts_r.append((x + wdt / 2 + wobble, y))
    md.polygon(pts_l + pts_r[::-1], fill=255)
    bead = w0 * 0.95
    by = y0 + run
    md.ellipse([x - bead, by - bead * 0.9, x + bead, by + bead * 1.1], fill=255)
mask = mask.filter(ImageFilter.GaussianBlur(3.2)).point(lambda v: 255 if v > 118 else 0).filter(ImageFilter.GaussianBlur(0.8))
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
paint_shadow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
paint_shadow.putalpha(mask.point(lambda v: v * 120 // 255))
shadow_moved = Image.new("RGBA", (W, H), (0, 0, 0, 0))
shadow_moved.paste(paint_shadow, (int(size * 0.012), int(size * 0.02)))
canvas = Image.alpha_composite(canvas, shadow_moved.filter(ImageFilter.GaussianBlur(size * 0.02)))
canvas = Image.alpha_composite(canvas, paint)

# Its words, in the middle of the band.
label = layer()
lb = ImageDraw.Draw(label)
lf_size = round(band_h * 0.62)
lf = font("Montserrat-ExtraBold.ttf", lf_size)
text_w = lb.textlength("animator studio", font=lf)
band_mid = base_y + band_gap + band_h * 0.5
lb.text((W / 2 - text_w / 2 + (t0x + t1x) * SCALE / 2 - W / 2, band_mid), "animator studio", font=lf, fill=INK + (255,), anchor="lm")
canvas = Image.alpha_composite(canvas, label)

# BUILT BY, on a strip of paper, centred under the band (the drips run past its sides).
strip = layer()
st = ImageDraw.Draw(strip)
cs = round(strip_h * 0.50)
light_f, bold_f = font("Montserrat-SemiBold.ttf", cs), font("Montserrat-ExtraBold.ttf", cs)
part1, part2 = "BUILT BY ", CREDITS
w1, w2 = st.textlength(part1, font=light_f), st.textlength(part2, font=bold_f)
pad = strip_h * 0.36
sx0 = W / 2 - (w1 + w2) / 2 - pad
sy0 = base_y + band_gap + band_h + credits_gap
st.rounded_rectangle([sx0, sy0, sx0 + w1 + w2 + 2 * pad, sy0 + strip_h], radius=strip_h * 0.11, fill=(250, 250, 248, 255))
st.text((sx0 + pad, sy0 + strip_h / 2), part1, font=light_f, fill=(60, 60, 66, 255), anchor="lm")
st.text((sx0 + pad + w1, sy0 + strip_h / 2), part2, font=bold_f, fill=INK + (255,), anchor="lm")
strip_shadow = layer()
ImageDraw.Draw(strip_shadow).rounded_rectangle([sx0 + 10, sy0 + 16, sx0 + w1 + w2 + 2 * pad + 10, sy0 + strip_h + 16], radius=strip_h * 0.11, fill=(0, 0, 0, 120))
canvas = Image.alpha_composite(canvas, strip_shadow.filter(ImageFilter.GaussianBlur(14)))
canvas = Image.alpha_composite(canvas, strip.filter(ImageFilter.GaussianBlur(0.6)))

# ---- grain over the concrete (not over the words) ---------------------------------------------------------------
final = canvas.convert("RGB")
noise = np.asarray(Image.effect_noise((W, H), 22)).astype(np.float32) - 128
clean = np.asarray(type_layer.split()[3].filter(ImageFilter.MaxFilter(9))).astype(np.float32) / 255.0
out = np.asarray(final).astype(np.float32) + (noise * 0.22 * (1 - clean))[..., None]
final = Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))
final.save(OUT_FULL)
final.resize((3072, round(3072 * H / W)), Image.LANCZOS).save(OUT_ASSET, quality=93, subsampling=0, optimize=True)
final.resize((1584, round(1584 * H / W)), Image.LANCZOS).save(PREVIEW / "loading_concrete.jpg", quality=90)
final.crop(((W - SEEN) // 2, 0, (W + SEEN) // 2, H)).resize((1280, 720), Image.LANCZOS).save(PREVIEW / "loading_concrete_169.jpg", quality=90)
final.crop((int(x0 + word_w * 0.25), int(base_y - cap_h * 1.1), int(x0 + word_w * 0.75), int(sy0 + strip_h * 1.2))).resize((1200, 900), Image.LANCZOS).save(
    PREVIEW / "loading_concrete_zoom.jpg", quality=90)
print("ok", size, round(word_w), OUT_FULL)
