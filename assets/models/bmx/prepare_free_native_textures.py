"""Encode the supplied bike's colour atlas for the existing native material.

This is a private, offline input to build_native_bike_pack. It never installs a
pack. Normal XY is deliberately flat until the native tangent-space convention
is visually verified; the source roughness/metallic maps are not substituted
into the donor's unknown packed-mask channels.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct

import numpy as np
from PIL import Image


def sha(data):
    return hashlib.sha256(data).hexdigest()


def encode(image, codec):
    buffer = io.BytesIO()
    image.save(buffer, format='DDS', pixel_format=codec)
    dds = buffer.getvalue()
    if dds[:4] != b'DDS ' or struct.unpack_from('<I', dds, 4)[0] != 124:
        raise ValueError('Unexpected DDS container from encoder')
    fourcc = dds[84:88]
    header_size = 148 if fourcc == b'DX10' else 128
    if (codec == 'DXT1' and fourcc != b'DXT1') or (codec == 'BC5' and fourcc != b'DX10'):
        raise ValueError('Encoder changed block format')
    if codec == 'BC5' and struct.unpack_from('<I', dds, 128)[0] not in (82, 83):
        # Pillow labels its unsigned BC5 payload TYPELESS (82); the native
        # resource supplies the sampling format. Signed BC5 (84) is rejected.
        raise ValueError('Expected typeless/unsigned BC5 DXGI format')
    width, height = image.size
    block_size = 8 if codec == 'DXT1' else 16
    raw = dds[header_size:]
    if len(raw) != max(1, (width+3)//4)*max(1, (height+3)//4)*block_size:
        raise ValueError('Block payload size differs from dimensions')
    decoded = Image.open(io.BytesIO(dds)).convert('RGB')
    difference = np.asarray(decoded).astype(np.float64)-np.asarray(image).astype(np.float64)
    if codec == 'BC5':
        difference = difference[:, :, :2]  # BC5 carries XY; Z is reconstructed by the shader.
    return raw, {'width': width, 'height': height, 'bytes': len(raw), 'sha256': sha(raw),
                 'decoded_rmse': float(np.sqrt(np.mean(difference*difference))),
                 'decoded_max_error': int(np.abs(difference).max())}


def write_texture(output, name, image, codec, ordinal, all_mips):
    width, height = image.size
    payload, rows = bytearray(), []
    while True:
        raw, row = encode(image, codec)
        row['offset'] = len(payload)
        payload.extend(raw)
        rows.append(row)
        if not all_mips or image.size == (1, 1):
            break
        # Standard colour mip filtering, not a claim about the source shader's
        # linear-light pipeline. UV atlas gutters are retained at full size.
        image = image.resize((max(1, image.width//2), max(1, image.height//2)), Image.Resampling.BOX)
    if len(rows) > 15:
        raise ValueError('Native mip table is limited to 15 entries')
    sizes = [row['bytes'] for row in rows]+[0]*(15-len(rows))
    info = struct.pack('<4s19I', b'BMXT', width, height, ordinal, len(rows), *sizes)
    (output/f'{name}.info').write_bytes(info)
    (output/f'{name}.pixels').write_bytes(payload)
    return {'name': name, 'format': ordinal, 'width': width, 'height': height,
            'mips': rows, 'info_sha256': sha(info), 'pixels_sha256': sha(payload),
            'decoded_bytes': len(payload)}


def prepare(atlas, output):
    if output.exists():
        raise ValueError('Output must be a NEW separate directory')
    image = Image.open(atlas/'albedo.png').convert('RGBA')
    if image.size != (4096, 2048) or image.getchannel('A').getextrema() != (255, 255):
        raise ValueError('Expected opaque two-tile 4096x2048 colour atlas')
    output.mkdir(parents=True)
    colour = write_texture(output, 'albedo', image.convert('RGB'), 'DXT1', 55, True)
    normal = write_texture(output, 'normal', Image.new('RGB', (4, 4), (128, 128, 255)), 'BC5', 63, False)
    if colour['mips'][0]['decoded_rmse'] > 16 or normal['mips'][0]['decoded_max_error'] != 0:
        raise ValueError('Unexpected block compression error')
    report = {'atlas_sha256': sha((atlas/'albedo.png').read_bytes()), 'textures': [colour, normal],
              'limits': ['Source colour/UV layout retained with BC1 compression and filtered mipmaps.',
                         'BC5 normal is flat; source normal green-channel convention remains unverified.',
                         'Donor packed mask/shader retained; source metallic and roughness maps are not native-bound.',
                         'Native header, CAS boundaries, chunk metadata and TextureAsset bindings are assembled separately.']}
    (output/'textures.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps({'output': str(output), 'colour_bytes': colour['decoded_bytes'],
                      'mips': len(colour['mips']), 'normal_bytes': normal['decoded_bytes']}))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--atlas', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    prepare(args.atlas, args.output)
