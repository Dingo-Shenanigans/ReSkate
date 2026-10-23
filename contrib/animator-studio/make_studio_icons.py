"""The Animator Studio's settings icons, in skate.'s own menu style: a white illustration with black line work, black
halftone shading on its shadow side, rough, worn edges and the odd drip. Written to assets/animator_studio/icon_*.png
(transparent, 512 x 512) and built into ReSkate.dll (cmake/Runtime.cmake).

    python contrib/animator-studio/make_studio_icons.py [out_dir]
"""
import math
import random
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter

S = 1024          # drawn at this size, saved at half
LINE = 30         # the black line round each shape
OUT = 512


def canvas():
    return Image.new('L', (S, S), 0)


def rotate(mask, degrees, centre=None):
    return mask.rotate(degrees, resample=Image.BICUBIC, center=centre)


def dilate(mask, px):
    return mask.filter(ImageFilter.MaxFilter(px * 2 + 1)) if px > 0 else mask


def erode(mask, px):
    return mask.filter(ImageFilter.MinFilter(px * 2 + 1)) if px > 0 else mask


def blur_noise(seed, cells, sharp=False):
    rnd = random.Random(seed)
    small = Image.new('L', (cells, cells))
    small.putdata([rnd.randrange(256) for _ in range(cells * cells)])
    big = small.resize((S, S), Image.BICUBIC)
    return big


class Icon:
    """Layers: `body` white, `ink` black line work drawn over it, `shade` 0-255 how dark the halftone is there."""

    def __init__(self, seed):
        self.body = canvas()
        self.ink = canvas()
        self.seed = seed
        self.drips = []  # (x, length, width): hanging off the lowest edge of the body at x

    def draw_body(self):
        return ImageDraw.Draw(self.body)

    def draw_ink(self):
        return ImageDraw.Draw(self.ink)

    def render(self, light=(-0.6, -0.8), shade_from=0.56, dot=20):
        rnd = random.Random(self.seed)
        body = self.body
        # Drips hang off the lowest edge, white with their own outline: a run of paint, thinning, with a drop at its end.
        for (x, length, width) in self.drips:
            column = [body.getpixel((x, y)) for y in range(S)]
            bottom = max((y for y, v in enumerate(column) if v > 128), default=None)
            if bottom is None:
                continue
            d = ImageDraw.Draw(body)
            top = bottom - width * 0.6
            # A run of paint: wide where it leaves the edge (a curve out of it either side), thinning, a fat drop at its end.
            d.ellipse([x - width * 1.1, top - width * 0.5, x + width * 1.1, top + width * 0.7], fill=255)
            d.polygon([(x - width * 0.5, top), (x + width * 0.5, top), (x + width * 0.36, top + length), (x - width * 0.36, top + length)], fill=255)
            d.ellipse([x - width * 0.62, top + length - width * 0.5, x + width * 0.62, top + length + width * 0.74], fill=255)
        bbox = body.getbbox() or (0, 0, S, S)
        # The shading: dark away from the light, as halftone dots, and only past `shade_from` of the way across.
        lx, ly = light
        norm = math.hypot(lx, ly)
        lx, ly = lx / norm, ly / norm
        x0, y0, x1, y1 = bbox
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        half = max(x1 - x0, y1 - y0) / 2
        dots = canvas()
        dd = ImageDraw.Draw(dots)
        rough = blur_noise(self.seed + 7, 9)
        for gy in range(0, S, dot):
            for gx in range(0, S, dot):
                ox = gx + (dot / 2 if (gy // dot) % 2 else 0)
                along = -((ox - cx) * lx + (gy - cy) * ly) / half  # -1 lit side, 1 shadow side
                t = (along + 1) / 2
                t = (t - shade_from) / (1 - shade_from)
                t += (rough.getpixel((min(S - 1, int(ox)), min(S - 1, gy))) - 128) / 255 * 0.35
                if t <= 0.02:
                    continue
                r = min(dot * 0.56, dot * 0.5 * math.sqrt(min(1.0, t)) * 1.05)
                dd.ellipse([ox - r, gy - r, ox + r, gy + r], fill=255)
        dots = ImageChops.multiply(dots, erode(body, 6))
        # Wear: specks knocked out of the white, and its edge chewed.
        specks = canvas()
        sd = ImageDraw.Draw(specks)
        for _ in range(620):
            x, y = rnd.randrange(x0, x1 + 1), rnd.randrange(y0, y1 + 1)
            r = rnd.choice([2, 2, 3, 3, 4, 5, 7])
            sd.ellipse([x - r, y - r, x + r, y + r], fill=255)
        scratch = canvas()
        sc = ImageDraw.Draw(scratch)
        for _ in range(26):
            x, y = rnd.randrange(x0, x1 + 1), rnd.randrange(y0, y1 + 1)
            a = rnd.uniform(-0.5, 0.5) + (math.pi / 4 if rnd.random() < 0.5 else 0)
            length = rnd.randrange(30, 160)
            sc.line([x, y, x + math.cos(a) * length, y + math.sin(a) * length], fill=255, width=rnd.choice([2, 3, 4]))
        wear = ImageChops.lighter(specks, scratch)
        wear = ImageChops.multiply(wear, erode(body, 10))
        # The outline: the body grown by the line's width, roughened.
        outline = dilate(body, LINE // 2)
        chew = blur_noise(self.seed + 3, 140).point(lambda v: 255 if v > 150 else 0)
        edge = ImageChops.subtract(dilate(outline, 4), erode(outline, 6))
        outline = ImageChops.subtract(outline, ImageChops.multiply(edge, chew))
        white = ImageChops.subtract(body, ImageChops.lighter(dots, wear))
        white = ImageChops.subtract(white, self.ink)
        # Together: black where the outline or the ink is, white where the body shows through.
        alpha = ImageChops.lighter(outline, ImageChops.lighter(body, self.ink))
        rgb = Image.merge('RGB', [white] * 3)
        out = Image.merge('RGBA', [*rgb.split(), alpha])
        return out.resize((OUT, OUT), Image.LANCZOS)


def thick(draw, points, width, fill=255):
    draw.line(points, fill=fill, width=width, joint='curve')
    for x, y in (points[0], points[-1]):
        draw.ellipse([x - width / 2, y - width / 2, x + width / 2, y + width / 2], fill=fill)


# ---- TRICKS: a board turning over ------------------------------------------------------------------
def board():
    icon = Icon(11)
    layer = canvas()
    d = ImageDraw.Draw(layer)
    # The deck, from above at an angle: long and round at both ends, the kicks a shade narrower.
    d.rounded_rectangle([170, 420, 860, 600], radius=90, fill=255)
    ink = canvas()
    di = ImageDraw.Draw(ink)
    # Grip edge and the bolts.
    di.rounded_rectangle([200, 446, 830, 574], radius=66, outline=255, width=12)
    for bx in (330, 380, 650, 700):
        for by in (482, 538):
            di.ellipse([bx - 9, by - 9, bx + 9, by + 9], fill=255)
    # Trucks and wheels below.
    wheels = canvas()
    dw = ImageDraw.Draw(wheels)
    for tx in (355, 675):
        dw.rounded_rectangle([tx - 70, 590, tx + 70, 632], radius=18, fill=255)
        for wx, wy in ((tx - 30, 650), (tx + 28, 668)):  # a pair, the far one a little behind
            dw.ellipse([wx - 44, wy - 40, wx + 44, wy + 40], fill=255)
    wink = canvas()
    dwi = ImageDraw.Draw(wink)
    for tx in (355, 675):
        dwi.line([tx - 70, 611, tx + 70, 611], fill=255, width=10)   # the truck's hanger against its base
        for wx, wy in ((tx + 28, 668), (tx - 30, 650)):              # each wheel's edge, the near one over the far
            dwi.ellipse([wx - 44, wy - 40, wx + 44, wy + 40], outline=255, width=11)
            dwi.ellipse([wx - 12, wy - 11, wx + 12, wy + 11], fill=255)
    layer = ImageChops.lighter(layer, wheels)
    ink = ImageChops.lighter(ink, wink)
    icon.body = rotate(layer, 24, (512, 540))
    icon.ink = rotate(ink, 24, (512, 540))
    icon.drips = [(300, 130, 58), (400, 80, 46)]
    # Motion: a swoosh round the tail.
    d2 = icon.draw_body()
    for i, r in enumerate((420, 470)):
        d2.arc([512 - r, 520 - r, 512 + r, 520 + r], 196 + i * 4, 238 - i * 6, fill=255, width=22 - i * 6)
    return icon.render(light=(-0.7, -0.7), shade_from=0.42)


# ---- STUDIO: the filmer's camcorder, fisheye on ------------------------------------------------------
def camcorder():
    icon = Icon(23)
    d = icon.draw_body()
    # Body, handle over it, the viewfinder at the back.
    d.rounded_rectangle([300, 420, 760, 700], radius=46, fill=255)
    d.rounded_rectangle([340, 330, 700, 380], radius=24, fill=255)
    d.rectangle([370, 370, 410, 430], fill=255)
    d.rectangle([640, 370, 680, 430], fill=255)
    d.rounded_rectangle([740, 450, 860, 540], radius=30, fill=255)
    # The fisheye: big and round out of the front.
    d.ellipse([120, 400, 420, 700], fill=255)
    di = icon.draw_ink()
    di.ellipse([150, 430, 390, 670], outline=255, width=14)
    di.ellipse([200, 480, 340, 620], fill=255)
    di.ellipse([228, 506, 270, 548], fill=0)  # its shine
    # Details: the record light, a dial, the lines on the side.
    di.ellipse([690, 470, 724, 504], fill=255)
    di.ellipse([560, 560, 650, 650], outline=255, width=12)
    for y in (470, 500, 530):
        di.line([470, y, 600, y], fill=255, width=10)
    di.line([760, 495, 840, 495], fill=255, width=10)
    icon.drips = [(560, 130, 58), (660, 80, 46)]
    return icon.render(light=(-0.6, -0.8), shade_from=0.40)


# ---- SKATER: a skate shoe ------------------------------------------------------------------------------
def shoe():
    icon = Icon(37)
    d = icon.draw_body()
    # A low-top, side on: the round toe at the left, the throat rising to the collar, the heel at the right.
    upper = [(170, 640), (190, 575), (260, 535), (380, 515), (470, 470), (540, 410), (610, 380), (690, 372), (760, 382),
             (815, 410), (850, 470), (868, 560), (874, 640)]
    d.polygon(upper, fill=255)
    d.ellipse([140, 540, 360, 700], fill=255)
    d.rounded_rectangle([120, 628, 900, 722], radius=46, fill=255)  # the sole, thick all along
    di = icon.draw_ink()
    di.line([128, 640, 892, 640], fill=255, width=14)
    for x in range(176, 860, 48):
        di.line([x, 682, x + 24, 682], fill=255, width=8)
    di.arc([150, 525, 440, 760], 196, 292, fill=255, width=12)            # the toe cap
    # The side stripe: a wave from under the laces back to the heel.
    thick(di, [(430, 618), (520, 572), (600, 560), (680, 520), (760, 470), (838, 446)], 30)
    # Laces across the throat, and the collar's padding.
    for x, y in [(410, 520), (455, 497), (500, 470), (542, 440)]:
        di.line([x - 26, y + 22, x + 30, y - 24], fill=255, width=13)
    di.arc([600, 360, 880, 520], 190, 300, fill=255, width=12)
    icon.drips = [(610, 120, 56)]
    return icon.render(light=(-0.5, -0.9))


# ---- PACKS: a VHS tape, the part on it ------------------------------------------------------------------
def tape():
    icon = Icon(51)
    layer = canvas()
    d = ImageDraw.Draw(layer)
    d.rounded_rectangle([150, 330, 870, 720], radius=34, fill=255)
    ink = canvas()
    di = ImageDraw.Draw(ink)
    # The label strip and the window with its two reels.
    di.rounded_rectangle([210, 380, 810, 470], radius=12, outline=255, width=12)
    for x in range(240, 760, 70):
        di.line([x, 425, x + 40, 425], fill=255, width=8)
    di.rounded_rectangle([300, 520, 720, 650], radius=40, outline=255, width=14)
    for cx in (395, 625):
        di.ellipse([cx - 52, 533, cx + 52, 637], fill=255)
        layer_hole = ImageDraw.Draw(ink)
        layer_hole.ellipse([cx - 20, 565, cx + 20, 605], fill=0)
        for a in range(0, 360, 60):
            r = math.radians(a)
            layer_hole.line([cx, 585, cx + math.cos(r) * 34, 585 + math.sin(r) * 34], fill=0, width=8)
    di.line([200, 690, 820, 690], fill=255, width=10)
    icon.body = rotate(layer, -10, (512, 525))
    icon.ink = rotate(ink, -10, (512, 525))
    icon.drips = [(330, 130, 58), (690, 80, 46)]
    return icon.render(light=(-0.7, -0.7), shade_from=0.42)


def main():
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2] / 'assets' / 'animator_studio'
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, make in (('studio', camcorder), ('skater', shoe), ('tricks', board), ('packs', tape)):
        make().save(out_dir / f'icon_{name}.png', optimize=True)
        print('wrote', out_dir / f'icon_{name}.png')


if __name__ == '__main__':
    main()
