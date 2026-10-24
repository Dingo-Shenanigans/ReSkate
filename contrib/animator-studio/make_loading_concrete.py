"""The Animator Studio's loading picture, concrete only: no skater. The loading plate's clean left half (the wall and the
floor, warm towards the middle) with its mirror beside it, lit from the middle, and the reskate. lockup centred on it:
the wordmark, the blue tape "animator studio" and BUILT BY. The plate is made by make_loading_plate.py.

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

rnd = random.Random(7)
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
# A cold light on the wall behind the words.
leak = layer()
ImageDraw.Draw(leak).ellipse([W * 0.5 - 2100, H * 0.18 - 900, W * 0.5 + 2100, H * 0.18 + 1100], fill=(1, 110, 255, 34))
canvas = Image.alpha_composite(canvas, leak.filter(ImageFilter.GaussianBlur(420)))


def spaced_width(draw, f, text, track):
    return sum(draw.textlength(ch, font=f) for ch in text) + track * (len(text) - 1)


# ---- the lockup, centred ------------------------------------------------------------------------------------
probe = ImageDraw.Draw(layer())
word = "reskate."
size = 600
while True:
    wf = font("Montserrat-ExtraBold.ttf", size)
    track = -0.035 * size
    if spaced_width(probe, wf, word, track) <= 2500:
        break
    size -= 6
word_w = spaced_width(probe, wf, word, track)
x0, base_y = W / 2 - word_w / 2, H * 0.5 + 60
type_layer = layer()
tl = ImageDraw.Draw(type_layer)
shadow = layer()
sd = ImageDraw.Draw(shadow)
x = x0
for ch in word:
    sd.text((x + 14, base_y + 18), ch, font=wf, fill=(0, 0, 0, 150), anchor="ls")
    tl.text((x, base_y), ch, font=wf, fill=PAPER + (255,), anchor="ls")
    x += tl.textlength(ch, font=wf) + track
word_right = x - track
canvas = Image.alpha_composite(canvas, shadow.filter(ImageFilter.GaussianBlur(18)))
canvas = Image.alpha_composite(canvas, type_layer)

# The blue tape: a dry brush stroke, ragged, splattered, dripping (as the old loading picture's).
tape = layer()
tp = ImageDraw.Draw(tape)
t0x, t1x, t0y, t1y = x0 + 240, word_right - 140, base_y + 86, base_y + 346
phase = [rnd.uniform(0, 6.28) for _ in range(6)]


def wander(s, k):
    return 14 * math.sin(s * 5.3 + phase[k]) + 8 * math.sin(s * 13.7 + phase[k + 1]) + 4 * math.sin(s * 31 + phase[k + 2])


top, bottom = [], []
for i in range(241):
    s = i / 240
    xs = t0x + (t1x - t0x) * s
    top.append((xs, t0y + wander(s, 0) + (rnd.uniform(0, 7) if rnd.random() < 0.7 else rnd.uniform(8, 18))))
    bottom.append((xs, t1y + wander(s, 3) - (rnd.uniform(0, 7) if rnd.random() < 0.7 else rnd.uniform(8, 20))))
tp.polygon(top + bottom[::-1], fill=BLUE + (255,))
for end, direction in ((t0x, -1), (t1x, 1)):
    for _ in range(9):
        yk = rnd.uniform(t0y + 20, t1y - 20)
        length = rnd.uniform(20, 110) if direction > 0 else rnd.uniform(15, 70)
        tp.line([(end - direction * 10, yk), (end + direction * length, yk + rnd.uniform(-4, 4))], fill=BLUE + (255,), width=rnd.randint(14, 34))
    for _ in range(6):
        yk = rnd.uniform(t0y + 20, t1y - 20)
        r = rnd.uniform(26, 60)
        xk = end + direction * rnd.uniform(-10, 30)
        tp.ellipse([xk - r, yk - r, xk + r, yk + r], fill=BLUE + (255,))
for _ in range(170):
    near_end = rnd.random() < 0.6
    xk = rnd.uniform(t1x - 160, t1x + 300) if near_end else rnd.uniform(t0x - 300, t0x + 160)
    yk = rnd.uniform(t0y - 80, t1y + 100)
    if t0y < yk < t1y and t0x < xk < t1x:
        continue
    r = rnd.choice([3, 4, 5, 6, 8, 10, 14])
    tp.ellipse([xk - r, yk - r, xk + r, yk + r], fill=BLUE + (255,))
for xd, length, width in ((t0x + 70, 70, 30), (t0x + 340, 40, 22), (t1x - 380, 64, 28), (t1x - 90, 150, 26)):
    tp.polygon([(xd - width / 2, t1y - 20), (xd + width / 2, t1y - 20), (xd + width * 0.3, t1y + length), (xd - width * 0.3, t1y + length)],
               fill=BLUE + (255,))
    tp.ellipse([xd - width * 0.6, t1y + length - width * 0.5, xd + width * 0.6, t1y + length + width * 0.7], fill=BLUE + (255,))
canvas = Image.alpha_composite(canvas, tape)
label = layer()
lb = ImageDraw.Draw(label)
lf = font("Montserrat-ExtraBold.ttf", 182)
text_w = lb.textlength("animator studio", font=lf)
lb.text(((t0x + t1x) / 2 - text_w / 2, (t0y + t1y) / 2 + 4), "animator studio", font=lf, fill=INK + (255,), anchor="lm")
canvas = Image.alpha_composite(canvas, label)

# BUILT BY, on a strip of paper, centred under the tape.
strip = layer()
st = ImageDraw.Draw(strip)
light_f, bold_f = font("Montserrat-SemiBold.ttf", 68), font("Montserrat-ExtraBold.ttf", 68)
part1, part2 = "BUILT BY ", CREDITS
w1, w2 = st.textlength(part1, font=light_f), st.textlength(part2, font=bold_f)
sx0 = W / 2 - (w1 + w2) / 2 - 48
sy0 = t1y + 110
st.rounded_rectangle([sx0, sy0, sx0 + w1 + w2 + 96, sy0 + 132], radius=14, fill=(250, 250, 248, 255))
st.text((sx0 + 48, sy0 + 66), part1, font=light_f, fill=(60, 60, 66, 255), anchor="lm")
st.text((sx0 + 48 + w1, sy0 + 66), part2, font=bold_f, fill=INK + (255,), anchor="lm")
canvas = Image.alpha_composite(canvas, strip.filter(ImageFilter.GaussianBlur(0.6)))

# ---- grain over the concrete (not over the words) ---------------------------------------------------------------
final = canvas.convert("RGB")
noise = np.asarray(Image.effect_noise((W, H), 22)).astype(np.float32) - 128
clean = np.asarray(type_layer.split()[3].filter(ImageFilter.MaxFilter(9))).astype(np.float32) / 255.0
out = np.asarray(final).astype(np.float32) + (noise * 0.22 * (1 - clean))[..., None]
final = Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))
final.save(OUT_FULL)
final.resize((3072, round(3072 * H / W)), Image.LANCZOS).save(OUT_ASSET, quality=93, subsampling=0, optimize=True)
preview = Path(r"C:\Users\Fourt\AppData\Local\Temp\claude\C--Users-Fourt\2ab0674e-9ffe-4afb-a488-7a96406c2044\scratchpad\thumb")
final.resize((1584, round(1584 * H / W)), Image.LANCZOS).save(preview / "loading_concrete.jpg", quality=90)
crop = int(H * 16 / 9)
final.crop(((W - crop) // 2, 0, (W + crop) // 2, H)).resize((1280, 720), Image.LANCZOS).save(preview / "loading_concrete_169.jpg", quality=90)
print("ok", OUT_FULL, OUT_ASSET)
