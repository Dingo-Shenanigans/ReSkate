"""Independent native texture-input descriptor/block audit, without a game."""
from pathlib import Path
import argparse
import hashlib
import json
import math
import struct
import numpy as np
from PIL import Image


def require(value, reason):
    if not value:
        raise ValueError(reason)


def descriptor(info, pixels, normal=False):
    require(len(info) == 80, 'descriptor length')
    magic, width, height, ordinal, count, *sizes = struct.unpack('<4s19I', info)
    require(magic == b'BMXT', 'descriptor magic')
    expected = (4, 4, 63, 1) if normal else (4096, 2048, 55, 13)
    require((width, height, ordinal, count) == expected, 'private texture dimensions/format/count')
    require(count <= math.floor(math.log2(max(width, height)))+1, 'excess mip count')
    rows = []
    offset = 0
    for i, size in enumerate(sizes):
        w, h = max(1, width >> i), max(1, height >> i)
        expected_size = max(1, (w+3)//4)*max(1, (h+3)//4)*(16 if normal else 8) if i < count else 0
        require(size == expected_size, 'mip byte count')
        if i < count:
            rows.append({'width': w, 'height': h, 'bytes': size, 'offset': offset})
        offset += size
    require(offset == len(pixels), 'pixel payload length')
    return rows


def bc1_opaque(raw, width, height):
    blocks = np.frombuffer(raw, np.uint8).reshape(-1, 8).astype(np.uint32)
    first = blocks[:, 0] | (blocks[:, 1] << 8)
    second = blocks[:, 2] | (blocks[:, 3] << 8)
    bits = blocks[:, 4] | (blocks[:, 5] << 8) | (blocks[:, 6] << 16) | (blocks[:, 7] << 24)
    block_width = max(1, (width+3)//4)
    x, y = np.arange(len(blocks)) % block_width, np.arange(len(blocks))//block_width
    transparent = 0
    for i in range(16):
        actual = (x*4+i % 4 < width) & (y*4+i//4 < height)
        transparent += int(np.sum(actual & (first <= second) & (((bits >> (2*i)) & 3) == 3)))
    return transparent


def sample_bc1(raw, image):
    """Literal BC1 palette/index decode, independent of Pillow's DDS decoder."""
    height, width = image.shape[:2]
    blocks = np.frombuffer(raw, np.uint8).reshape(-1, 8)
    selected = np.unique(np.linspace(0, len(blocks)-1, 2048).astype(np.int64))
    b = blocks[selected].astype(np.uint32)
    c0, c1 = b[:, 0] | (b[:, 1] << 8), b[:, 2] | (b[:, 3] << 8)
    def rgb(value):
        r, g, blue = (value >> 11) & 31, (value >> 5) & 63, value & 31
        return np.stack(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (blue << 3) | (blue >> 2)), axis=1).astype(np.int32)
    a, c = rgb(c0), rgb(c1)
    palette = np.stack((a, c, (2*a+c)//3, (a+2*c)//3), axis=1)
    three_colour = c0 <= c1
    palette[three_colour, 2] = (a[three_colour]+c[three_colour])//2
    palette[three_colour, 3] = 0
    bits = b[:, 4] | (b[:, 5] << 8) | (b[:, 6] << 16) | (b[:, 7] << 24)
    block_width = (width+3)//4
    bx, by = selected % block_width, selected//block_width
    errors = []
    for i in range(16):
        decoded = palette[np.arange(len(palette)), (bits >> (2*i)) & 3]
        errors.append(decoded-image[by*4+i//4, bx*4+i % 4].astype(np.int32))
    errors = np.concatenate(errors)
    return {'sampled_blocks': len(selected), 'sampled_pixels': len(errors),
            'rmse_rgb': float(np.sqrt(np.mean(errors.astype(np.float64)**2))),
            'maximum_rgb_error': int(np.abs(errors).max())}


def bc5_flat(raw):
    require(len(raw) == 16, 'flat normal is one BC5 block')
    components = []
    for block in (raw[:8], raw[8:]):
        a, b = block[:2]
        palette = [a, b]
        if a > b:
            palette.extend(((7-i)*a+i*b)//7 for i in range(1, 7))
        else:
            palette.extend(((5-i)*a+i*b)//5 for i in range(1, 5))
            palette.extend((0, 255))
        bits = int.from_bytes(block[2:], 'little')
        components.append([palette[(bits >> (3*i)) & 7] for i in range(16)])
    values = np.array(components).T
    require(np.all(values == 128), 'BC5 normal XY is not exactly flat128')
    return values.tolist()


def validate(root, atlas):
    report = {'scope': 'Offline native-input descriptor and independent BC1/BC5 block audit; no shader/PBR/native-loader claim', 'textures': []}
    for name in ('albedo', 'normal'):
        info, pixels = (root/f'{name}.info').read_bytes(), (root/f'{name}.pixels').read_bytes()
        rows = descriptor(info, pixels, name == 'normal')
        result = {'name': name, 'mips': rows, 'info_sha256': hashlib.sha256(info).hexdigest(),
                  'pixels_sha256': hashlib.sha256(pixels).hexdigest()}
        if name == 'albedo':
            for row in rows:
                raw = pixels[row['offset']:row['offset']+row['bytes']]
                row['transparent_actual_texels'] = bc1_opaque(raw, row['width'], row['height'])
                require(row['transparent_actual_texels'] == 0, 'opaque source acquired BC1 transparent texels')
            with Image.open(atlas/'albedo.png') as source:
                result['independent_source_samples'] = sample_bc1(pixels[:rows[0]['bytes']], np.asarray(source.convert('RGB')))
            require(result['independent_source_samples']['rmse_rgb'] < 16, 'independent colour decode differs excessively from supplied atlas')
        else:
            result['decoded_xy'] = bc5_flat(pixels)
        rejected = []
        wrong_magic = b'NOPE'+info[4:]
        wrong_size = bytearray(info);wrong_size[20] ^= 1
        for label, bad_info, bad_pixels in (('magic', wrong_magic, pixels), ('mip_size', wrong_size, pixels), ('truncated', info, pixels[:-1])):
            try:
                descriptor(bad_info, bad_pixels, name == 'normal')
            except ValueError:
                rejected.append(label)
            else:
                raise ValueError('Negative descriptor accepted: '+label)
        result['negative_rejections'] = rejected
        report['textures'].append(result)
        print(name, 'PASS', len(rows), 'mips', len(pixels), 'bytes', flush=True)
    report['atlas_sha256'] = hashlib.sha256((atlas/'albedo.png').read_bytes()).hexdigest()
    report['passed'] = True
    (root/'independent-texture-validation.json').write_text(json.dumps(report, indent=2)+'\n')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--textures', type=Path, required=True)
    parser.add_argument('--atlas', type=Path, required=True)
    args = parser.parse_args()
    validate(args.textures, args.atlas)
