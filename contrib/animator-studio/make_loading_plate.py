"""Backdrop 3 with its lettering taken off: the white wordmark, the blue tape and the credits strip are found by colour and
filled with the wall's own concrete - its texture from clean wall above, its light from the wall around the hole."""
import numpy as np
from PIL import Image, ImageFilter

SRC = r"C:\Users\Fourt\Documents\Reskate\Animator Studio backdrop 3.png"
OUT = r"C:\Users\Fourt\AppData\Local\Temp\claude\C--Users-Fourt\2ab0674e-9ffe-4afb-a488-7a96406c2044\scratchpad\backdrop\plate.png"

img = np.asarray(Image.open(SRC).convert('RGB')).astype(np.float32)
H, W, _ = img.shape
box = (840, 680, 3320, 1960)  # x0, y0, x1, y1: where the lettering is


def blur(a, r):
    """A box blur three times over (close to a gaussian), on float arrays, along both axes."""
    out = a.astype(np.float32)
    for _ in range(3):
        for axis in (0, 1):
            c = np.cumsum(out, axis=axis, dtype=np.float64)
            pad = [(0, 0)] * out.ndim
            pad[axis] = (1, 0)
            c = np.pad(c, pad)
            n = out.shape[axis]
            idx_hi = np.clip(np.arange(n) + r + 1, 0, n)
            idx_lo = np.clip(np.arange(n) - r, 0, n)
            hi = np.take(c, idx_hi, axis=axis)
            lo = np.take(c, idx_lo, axis=axis)
            count = (idx_hi - idx_lo).astype(np.float32)
            shape = [1] * out.ndim
            shape[axis] = n
            out = ((hi - lo) / count.reshape(shape)).astype(np.float32)
    return out


# The lettering: bright (wordmark, strip, splatter) or blue (tape), inside the box.
r, g, b = img[..., 0], img[..., 1], img[..., 2]
luma = 0.299 * r + 0.587 * g + 0.114 * b
mask = (luma > 118) | ((b > r + 30) & (b > 95))
inside = np.zeros_like(mask)
inside[box[1]:box[3], box[0]:box[2]] = True
mask &= inside
m = Image.fromarray((mask * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(25)).filter(ImageFilter.MaxFilter(15))
hole = np.asarray(m).astype(np.float32) / 255.0
soft = np.clip(blur(hole, 6) * 1.6, 0, 1)  # feathered, a touch wider than the hole

# The light: the wall round the hole, spread into it (normalised convolution).
valid = 1.0 - np.clip(blur(hole, 4) * 3, 0, 1)
light = np.zeros_like(img)
weight = blur(valid, 90)
for c in range(3):
    light[..., c] = blur(img[..., c] * valid, 90) / np.maximum(weight, 1e-4)

# The texture: clean wall above the lettering, mirrored down over the hole, from a band offset sideways so it does not
# simply repeat what is just above.
detail = img - blur(img, 26)
band_top, band_bottom = 40, 660
rows = np.arange(H)
offset = rows - box[1]
period = band_bottom - band_top
pp = np.abs((offset % (2 * period)) - period)  # ping-pong
src_rows = np.clip(band_bottom - pp, band_top, band_bottom - 1)
cols = np.clip(np.arange(W) + 260, 0, W - 1)
texture = detail[src_rows][:, cols]

filled = light + texture * 1.05
out = img * (1 - soft[..., None]) + filled * soft[..., None]
Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).save(OUT)
Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).resize((1584, 672), Image.LANCZOS).save(OUT.replace('.png', '_preview.jpg'), quality=90)
print('ok', W, H)
