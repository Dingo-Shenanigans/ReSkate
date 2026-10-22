"""The Animator Studio's loading picture: backdrop 3's kickflip, clean - the skater in a deep vignette, and the reskate. lockup
with its tape and credits. Its plate (the lettering taken off backdrop 3) is made by make_loading_plate.py."""
import math
import random
import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

HERE = r"C:\Users\Fourt\AppData\Local\Temp\claude\C--Users-Fourt\2ab0674e-9ffe-4afb-a488-7a96406c2044\scratchpad\backdrop"
FONTS = r"C:\Users\Fourt\Documents\Reskate\ReSkate pr\External\fonts"
OUT = r"C:\Users\Fourt\Documents\Reskate\Animator Studio backdrop 4.png"

base = Image.open(HERE + r"\plate.png").convert('RGB')
W, H = base.size
rnd = random.Random(7)

BLUE = (1, 131, 255)
BLUE_LIGHT = (90, 180, 255)
WHITE = (245, 245, 245)
PAPER = (244, 241, 233)
INK = (16, 18, 24)
RED, GREEN, ZBLUE = (236, 76, 72), (118, 212, 96), (74, 150, 255)


def font(name, size):
    return ImageFont.truetype(FONTS + '\\' + name, size)


def layer():
    return Image.new('RGBA', (W, H), (0, 0, 0, 0))


def glow(img, radius, strength=1.0):
    g = img.filter(ImageFilter.GaussianBlur(radius))
    if strength != 1.0:
        a = g.split()[3].point(lambda v: min(255, int(v * strength)))
        g.putalpha(a)
    return g


# ---- grade: a little cooler in the shadows, deeper at the edges ----------------------------------------
arr = np.asarray(base).astype(np.float32)
luma = arr.mean(axis=2, keepdims=True) / 255.0
cool = np.array([0.96, 0.99, 1.06], dtype=np.float32)
arr = arr * (cool * (1 - luma) + luma)  # shadows lean blue, highlights stay warm
yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
cx, cy = W * 0.70, H * 0.46
d = np.sqrt(((xx - cx) / (W * 0.50)) ** 2 + ((yy - cy) / (H * 0.70)) ** 2)
vignette = np.clip(1.10 - 0.62 * d ** 1.6, 0.18, 1.06)
# and the top and the bottom edges a touch darker again, as a lens does
edge = np.clip(1.0 - 0.35 * (np.abs(yy / H - 0.47) / 0.53) ** 3, 0.6, 1.0)
arr *= (vignette * edge)[..., None]
canvas = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8)).convert('RGBA')

# A cold light from the top left, behind the lockup.
leak = layer()
ld = ImageDraw.Draw(leak)
ld.ellipse([-900, -1300, 3400, 1500], fill=(1, 110, 255, 38))
canvas = Image.alpha_composite(canvas, leak.filter(ImageFilter.GaussianBlur(420)))

# ---- the lockup ----------------------------------------------------------------------------------------
type_layer = layer()
tl = ImageDraw.Draw(type_layer)
word = 'reskate.'
size = 560
wf = font('Montserrat-ExtraBold.ttf', size)
track = -0.035 * size


def word_width(f, text, track):
    return sum(tl.textlength(ch, font=f) for ch in text) + track * (len(text) - 1)


while word_width(wf, word, track) > 2330:
    size -= 6
    wf = font('Montserrat-ExtraBold.ttf', size)
    track = -0.035 * size
x0, base_y = 930, 1230
x = x0
shadow = layer()
sd = ImageDraw.Draw(shadow)
for ch in word:
    sd.text((x + 14, base_y + 18), ch, font=wf, fill=(0, 0, 0, 150), anchor='ls')
    tl.text((x, base_y), ch, font=wf, fill=PAPER + (255,), anchor='ls')
    x += tl.textlength(ch, font=wf) + track
word_right = x - track
canvas = Image.alpha_composite(canvas, shadow.filter(ImageFilter.GaussianBlur(18)))
# Worn: the letters take a little of a grain.
grain = np.asarray(Image.effect_noise((W // 4, H // 4), 60).resize((W, H), Image.BICUBIC)).astype(np.float32)
alpha = np.asarray(type_layer.split()[3]).astype(np.float32)
worn = alpha * np.clip(0.95 + (grain - 128) / 255 * 0.22, 0.86, 1.0)
type_layer.putalpha(Image.fromarray(worn.astype(np.uint8)))
canvas = Image.alpha_composite(canvas, type_layer)

# The blue tape: a dry brush stroke, ragged, streaked, splattered, dripping.
tape = layer()
tp = ImageDraw.Draw(tape)
t0x, t1x, t0y, t1y = x0 + 220, word_right - 120, base_y + 80, base_y + 330
top, bottom = [], []
phase = [rnd.uniform(0, 6.28) for _ in range(6)]
def wander(s, k):
    return (14 * math.sin(s * 5.3 + phase[k]) + 8 * math.sin(s * 13.7 + phase[k + 1]) + 4 * math.sin(s * 31 + phase[k + 2]))
for i in range(241):
    s = i / 240
    xx_ = t0x + (t1x - t0x) * s
    bite_top = rnd.uniform(0, 7) if rnd.random() < 0.7 else rnd.uniform(8, 18)
    bite_bottom = rnd.uniform(0, 7) if rnd.random() < 0.7 else rnd.uniform(8, 20)
    top.append((xx_, t0y + wander(s, 0) + bite_top))
    bottom.append((xx_, t1y + wander(s, 3) - bite_bottom))
tp.polygon(top + bottom[::-1], fill=BLUE + (255,))
# Ragged ends: streaks of the brush running out.
for end, direction in ((t0x, -1), (t1x, 1)):
    for k in range(9):
        yk = rnd.uniform(t0y + 20, t1y - 20)
        length = rnd.uniform(20, 110) if direction > 0 else rnd.uniform(15, 70)
        tp.line([(end - direction * 10, yk), (end + direction * length, yk + rnd.uniform(-4, 4))], fill=BLUE + (255,), width=rnd.randint(14, 34))
    # the end itself: round-ish lumps where the brush lifted
    for k in range(6):
        yk = rnd.uniform(t0y + 20, t1y - 20)
        r = rnd.uniform(26, 60)
        xk = end + direction * rnd.uniform(-10, 30)
        tp.ellipse([xk - r, yk - r, xk + r, yk + r], fill=BLUE + (255,))
# Bristle streaks: light and dark lines along the stroke.

# Splatter.
for k in range(150):
    near_end = rnd.random() < 0.75
    xk = rnd.uniform(t1x - 160, t1x + 280) if near_end else rnd.uniform(t0x - 140, t0x + 220)
    yk = rnd.uniform(t0y - 70, t1y + 90)
    if t0y < yk < t1y and t0x < xk < t1x:
        continue
    r = rnd.choice([3, 4, 5, 6, 8, 10, 14])
    tp.ellipse([xk - r, yk - r, xk + r, yk + r], fill=BLUE + (255,))
# Drips: runs of paint off the bottom edge.
for xd, length, width in ((t0x + 70, 70, 30), (t0x + 300, 40, 22), (t1x - 360, 64, 28), (t1x - 90, 150, 26)):
    tp.polygon([(xd - width / 2, t1y - 20), (xd + width / 2, t1y - 20), (xd + width * 0.3, t1y + length), (xd - width * 0.3, t1y + length)], fill=BLUE + (255,))
    tp.ellipse([xd - width * 0.6, t1y + length - width * 0.5, xd + width * 0.6, t1y + length + width * 0.7], fill=BLUE + (255,))
canvas = Image.alpha_composite(canvas, tape)
# Its words.
label = layer()
lb = ImageDraw.Draw(label)
lf = font('Montserrat-ExtraBold.ttf', 176)
text_w = lb.textlength('animator studio', font=lf)
lb.text(((t0x + t1x) / 2 - text_w / 2, (t0y + t1y) / 2 + 4), 'animator studio', font=lf, fill=INK + (255,), anchor='lm')
canvas = Image.alpha_composite(canvas, label)

# BUILT BY, on a strip of paper.
strip = layer()
st = ImageDraw.Draw(strip)
light_f, bold_f = font('Montserrat-SemiBold.ttf', 66), font('Montserrat-ExtraBold.ttf', 66)
part1, part2 = 'BUILT BY ', '22SH, FOURTH, NORMS, STEEZ'
w1, w2 = st.textlength(part1, font=light_f), st.textlength(part2, font=bold_f)
sx0 = (t0x + t1x) / 2 - (w1 + w2) / 2 - 46
sy0 = t1y + 92
st.rounded_rectangle([sx0, sy0, sx0 + w1 + w2 + 92, sy0 + 128], radius=14, fill=(250, 250, 248, 255))
st.text((sx0 + 46, sy0 + 64), part1, font=light_f, fill=(60, 60, 66, 255), anchor='lm')
st.text((sx0 + 46 + w1, sy0 + 64), part2, font=bold_f, fill=INK + (255,), anchor='lm')
canvas = Image.alpha_composite(canvas, strip.filter(ImageFilter.GaussianBlur(0.6)))

# ---- grain over all of it ------------------------------------------------------------------------------
final = canvas.convert('RGB')
noise = np.asarray(Image.effect_noise((W, H), 22)).astype(np.float32) - 128
out = np.asarray(final).astype(np.float32) + noise[..., None] * 0.22
final = Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))
final.save(OUT)
final.resize((1584, 672), Image.LANCZOS).save(HERE + r"\compose_preview.jpg", quality=92)
# How it shows on a 16:9 screen (the sides cropped).
crop = int(H * 16 / 9)
final.crop(((W - crop) // 2, 0, (W + crop) // 2, H)).resize((1280, 720), Image.LANCZOS).save(HERE + r"\compose_169.jpg", quality=90)
print('ok', OUT)
