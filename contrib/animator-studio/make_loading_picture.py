"""The Animator Studio's loading picture: backdrop 3's kickflip with the studio drawn into it - the skater's joint rig, the
gizmo on the board, the board's arc as keyframes with onion-skin ghosts - and the reskate. lockup with a studio timeline."""
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
cx, cy = W * 0.62, H * 0.48
d = np.sqrt(((xx - cx) / (W * 0.62)) ** 2 + ((yy - cy) / (H * 0.75)) ** 2)
vignette = np.clip(1.08 - 0.42 * d ** 1.8, 0.45, 1.05)
arr *= vignette[..., None]
canvas = Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8)).convert('RGBA')

# A cold light from the top left, behind the lockup.
leak = layer()
ld = ImageDraw.Draw(leak)
ld.ellipse([-900, -1300, 3400, 1500], fill=(1, 110, 255, 60))
canvas = Image.alpha_composite(canvas, leak.filter(ImageFilter.GaussianBlur(420)))

# ---- the skater's rig -------------------------------------------------------------------------------
J = {
    'head': (4560, 560), 'neck': (4470, 690), 'chest': (4250, 820), 'spine': (4060, 1000), 'hips': (3800, 1080),
    'r_shoulder': (3990, 640), 'r_elbow': (3650, 550), 'r_hand': (4260, 400),
    'l_shoulder': (4520, 840), 'l_elbow': (4800, 1010), 'l_hand': (5120, 1090),
    'b_knee': (3660, 1300), 'b_ankle': (3830, 1590), 'b_toe': (3950, 1730),
    'f_knee': (4580, 1340), 'f_ankle': (5100, 1560), 'f_toe': (5350, 1440),
}
BONES = [('head', 'neck'), ('neck', 'chest'), ('chest', 'spine'), ('spine', 'hips'), ('neck', 'r_shoulder'), ('r_shoulder', 'r_elbow'),
         ('r_elbow', 'r_hand'), ('neck', 'l_shoulder'), ('l_shoulder', 'l_elbow'), ('l_elbow', 'l_hand'), ('hips', 'b_knee'),
         ('b_knee', 'b_ankle'), ('b_ankle', 'b_toe'), ('hips', 'f_knee'), ('f_knee', 'f_ankle'), ('f_ankle', 'f_toe')]
rig = layer()
rd = ImageDraw.Draw(rig)
for a, b in BONES:
    rd.line([J[a], J[b]], fill=(0, 0, 0, 70), width=13)
for a, b in BONES:
    rd.line([J[a], J[b]], fill=(225, 238, 255, 120), width=5)
for name, (x, y) in J.items():
    r = 14
    rd.ellipse([x - r - 6, y - r - 6, x + r + 6, y + r + 6], fill=(0, 0, 0, 110))
    rd.ellipse([x - r, y - r, x + r, y + r], fill=(250, 250, 252, 235))
# The joint being posed: the front foot, with the studio's glowing ring and its name beside it.
fx, fy = J['f_ankle']
ring = layer()
rg = ImageDraw.Draw(ring)
for i, a in enumerate((60, 90, 140)):
    rr = 52 + i * 12
    rg.ellipse([fx - rr, fy - rr, fx + rr, fy + rr], outline=(1, 131, 255, a), width=10)
rig = Image.alpha_composite(rig, glow(ring, 14, 1.4))
rd = ImageDraw.Draw(rig)
rd.ellipse([fx - 46, fy - 46, fx + 46, fy + 46], outline=(80, 170, 255, 255), width=9)
rd.ellipse([fx - 12, fy - 12, fx + 12, fy + 12], fill=(230, 240, 255, 255))
canvas = Image.alpha_composite(canvas, glow(rig, 10, 0.6))
canvas = Image.alpha_composite(canvas, rig)


def chip(draw, at, label, fill, ink, size=40, pad=22):
    f = font('Montserrat-ExtraBold.ttf', size)
    w = draw.textlength(label, font=f)
    x, y = at
    h = size + pad
    draw.rounded_rectangle([x, y, x + w + pad * 2, y + h], radius=h // 2, fill=fill)
    draw.text((x + pad, y + h / 2), label, font=f, fill=ink, anchor='lm')
    return w + pad * 2


tags = layer()
td = ImageDraw.Draw(tags)
x = fx - 300
x += chip(td, (x, fy - 190), 'FRONT FOOT', (1, 131, 255, 245), (255, 255, 255, 255)) + 14
chip(td, (x, fy - 190), 'KEYED', (245, 245, 245, 240), INK + (255,))
canvas = Image.alpha_composite(canvas, tags)

# ---- the board's arc: keyframes, and the board where it was ----------------------------------------
board_c = (4300, 1900)
along = (1390 / 1438, 370 / 1438)
up = (along[1], -along[0])


def bezier(p0, p1, p2, p3, t):
    u = 1 - t
    return (u ** 3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t ** 3 * p3[0],
            u ** 3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t ** 3 * p3[1])


P0, P1, P2, P3 = (2950, 2330), (3550, 1700), (4800, 1650), (5750, 2330)
arc = layer()
ad = ImageDraw.Draw(arc)
steps = 140
for i in range(steps):
    t = i / steps
    if i % 2:
        continue
    a, b = bezier(P0, P1, P2, P3, t), bezier(P0, P1, P2, P3, t + 0.5 / steps)
    ad.line([a, b], fill=(160, 205, 255, 200), width=7)


def diamond(draw, c, r, fill, outline=None, width=0):
    x, y = c
    pts = [(x, y - r), (x + r, y), (x, y + r), (x - r, y)]
    draw.polygon(pts, fill=fill)
    if outline:
        draw.line(pts + [pts[0]], fill=outline, width=width, joint='curve')


small = font('Montserrat-ExtraBold.ttf', 34)
for t, frame in ((0.0, 0), (0.27, 6), (0.5, 14), (0.75, 21), (1.0, 28)):
    p = bezier(P0, P1, P2, P3, t)
    current = frame == 14
    diamond(ad, p, 30 if current else 22, (1, 131, 255, 255) if current else (250, 250, 252, 240), (10, 14, 20, 200), 6)
    ad.text((p[0], p[1] - 62), str(frame), font=small, fill=(220, 235, 255, 230), anchor='mm')
canvas = Image.alpha_composite(canvas, glow(arc, 8, 0.7))
canvas = Image.alpha_composite(canvas, arc)


def ghost_board(c, angle_deg, roll, alpha):
    """A wireframe board: the deck as seen turned `roll` about its length (0 grip up, 1 on its edge, 2 wheels up), and its wheels."""
    g = layer()
    gd = ImageDraw.Draw(g)
    length, width = 1380, 250
    a = math.radians(angle_deg)
    ux, uy = math.cos(a), math.sin(a)
    vx, vy = -uy, ux
    seen = abs(math.cos(roll * math.pi / 2)) * width * 0.5 + 18  # how wide the deck shows, turned that far
    # A stadium: the nose's round end, a side, the tail's round end, the other side.
    pts = []
    for k in range(25):
        th = -math.pi / 2 + math.pi * k / 24
        pts.append((length / 2 - seen + math.cos(th) * seen, math.sin(th) * seen))
    for k in range(25):
        th = math.pi / 2 + math.pi * k / 24
        pts.append((-(length / 2 - seen) + math.cos(th) * seen, math.sin(th) * seen))
    world = [(c[0] + ux * px + vx * py, c[1] + uy * px + vy * py) for px, py in pts]
    gd.polygon(world, fill=(1, 131, 255, int(alpha * 0.28)))
    gd.line(world + [world[0]], fill=(150, 200, 255, int(alpha)), width=7, joint='curve')
    # The wheels, on the side that faces down (or up, the board turned over).
    side = 1 if roll < 1 else -1
    for along_w in (-length * 0.30, length * 0.30):
        for spread in (-0.32, 0.32):
            px, py = along_w + spread * 180, side * (seen + 58)
            wx, wy = c[0] + ux * px + vx * py, c[1] + uy * px + vy * py
            gd.ellipse([wx - 44, wy - 44, wx + 44, wy + 44], outline=(150, 200, 255, int(alpha)), width=7)
    return g




# ---- the gizmo on the board ---------------------------------------------------------------------------
giz = layer()
gz = ImageDraw.Draw(giz)
R = 300
bx, by = board_c


def ellipse_points(major_dir, major, minor, n=120):
    mx, my = major_dir
    nx, ny = -my, mx
    return [(bx + mx * math.cos(t) * major + nx * math.sin(t) * minor, by + my * math.cos(t) * major + ny * math.sin(t) * minor)
            for t in (2 * math.pi * k / n for k in range(n + 1))]


rings = [(GREEN, along, R * 1.0, R * 0.30), (RED, along, R * 0.98, R * 0.90), (ZBLUE, up, R * 0.92, R * 0.24)]
for colour, direction, major, minor in rings:
    gz.line(ellipse_points(direction, major, minor), fill=colour + (235,), width=11, joint='curve')
# The arrows: up the board's up (green), to its nose (blue), out of it towards the camera (red).
for colour, (dx, dy), length in ((GREEN, up, 280), (ZBLUE, along, 300), (RED, (-0.55, 0.62), 190)):
    norm = math.hypot(dx, dy)
    dx, dy = dx / norm, dy / norm
    tip = (bx + dx * length, by + dy * length)
    gz.line([(bx, by), tip], fill=colour + (255,), width=12)
    s = 30
    gz.polygon([tip, (tip[0] - dx * s * 2 - dy * s, tip[1] - dy * s * 2 + dx * s), (tip[0] - dx * s * 2 + dy * s, tip[1] - dy * s * 2 - dx * s)],
               fill=colour + (255,))
gz.ellipse([bx - 34, by - 34, bx + 34, by + 34], outline=(80, 170, 255, 255), width=9)
gz.ellipse([bx - 11, by - 11, bx + 11, by + 11], fill=(235, 242, 255, 255))
gz.text((bx + 60, by + 58), 'OBJECT', font=font('Montserrat-ExtraBold.ttf', 34), fill=(255, 255, 255, 170))
canvas = Image.alpha_composite(canvas, glow(giz, 16, 0.9))
canvas = Image.alpha_composite(canvas, giz)

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
x0, base_y = 930, 1080
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

# ---- a studio timeline under it -----------------------------------------------------------------------
tl_layer = layer()
td = ImageDraw.Draw(tl_layer)
px0, px1, py0 = x0, word_right, sy0 + 200
row_h, label_w, header = 62, 300, 74
rows = ['HIPS', 'BOARD', 'FRONT FOOT']
py1 = py0 + header + row_h * len(rows) + 16
td.rounded_rectangle([px0, py0, px1, py1], radius=10, fill=(10, 11, 14, 205))
frames = 28
lane0, lane1 = px0 + label_w, px1 - 40
fx_of = lambda f: lane0 + (lane1 - lane0) * f / frames
tick_f = font('Montserrat-ExtraBold.ttf', 30)
for f in range(frames + 1):
    xf = fx_of(f)
    big = f % 7 == 0
    td.line([(xf, py0 + header - (24 if big else 12)), (xf, py0 + header - 4)], fill=(255, 255, 255, 230 if big else 110), width=4 if big else 3)
    if big:
        td.text((xf, py0 + 26), str(f), font=tick_f, fill=(255, 255, 255, 235), anchor='mm')
# The window the keys work in, and its flags.
w0, w1 = fx_of(3), fx_of(25)
td.rectangle([w0, py0 + header, w1, py1 - 8], fill=(1, 131, 255, 34))
flag_f = font('Montserrat-ExtraBold.ttf', 22)
for xw, right, name in ((w0, True, 'START ANIM.'), (w1, False, 'END ANIM.')):
    td.line([(xw, py0 + 6), (xw, py1 - 8)], fill=BLUE + (255,), width=6)
    fw = td.textlength(name, font=flag_f) + 26
    xa, xb = (xw, xw + fw) if right else (xw - fw, xw)
    td.polygon([(xa, py0 + 6), (xb, py0 + 6), (xb - (8 if right else -8), py0 + 22), (xb, py0 + 38), (xa, py0 + 38)], fill=BLUE + (255,))
    td.text(((xa + xb) / 2 - (4 if right else -4), py0 + 22), name, font=flag_f, fill=(8, 14, 24, 255), anchor='mm')
row_f = font('Montserrat-ExtraBold.ttf', 32)
keys = {'HIPS': [3, 9, 14, 20, 25], 'BOARD': [3, 6, 11, 14, 17, 25], 'FRONT FOOT': [3, 8, 14, 22, 25]}
for i, name in enumerate(rows):
    ry = py0 + header + 8 + i * row_h
    chosen = name == 'FRONT FOOT'
    td.rounded_rectangle([px0 + 12, ry, px0 + label_w - 14, ry + row_h - 8], radius=6, fill=BLUE + (255,) if chosen else (34, 35, 39, 240))
    td.text((px0 + 34, ry + (row_h - 8) / 2), name, font=row_f, fill=(255, 255, 255, 255), anchor='lm')
    td.rectangle([lane0, ry, lane1, ry + row_h - 8], fill=(44, 50, 62, 150) if chosen else (30, 31, 34, 140))
    for f in keys[name]:
        picked = f == 14
        diamond(td, (fx_of(f), ry + (row_h - 8) / 2), 17 if picked else 14, BLUE_LIGHT + (255,) if picked else (250, 250, 252, 240))
# The playhead.
xp = fx_of(14)
td.rectangle([xp - 30, py0 + 6, xp + 30, py0 + 46], fill=(10, 11, 14, 255))
td.text((xp, py0 + 26), '14', font=tick_f, fill=BLUE_LIGHT + (255,), anchor='mm')
td.line([(xp, py0 + 50), (xp, py1 - 4)], fill=BLUE + (255,), width=7)
td.polygon([(xp - 18, py0 + 48), (xp + 18, py0 + 48), (xp, py0 + 72)], fill=BLUE + (255,))
canvas = Image.alpha_composite(canvas, tl_layer)

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
print('ok', OUT, 'lockup right edge', int(word_right), 'timeline bottom', int(py1))
