"""Compare two Animator Studio trick libraries clip by clip: one built in game (or by dingosdk_anim_library_build), one by
these scripts. Same entries, same timing, poses within a hair, the same pop and landing on the game's tricks.

    python compare_libraries.py REFERENCE_LIBRARY NEW_LIBRARY
"""
import json
import sys
from pathlib import Path

import numpy as np

from repair_library_positions import read_clip


def main():
    old_dir, new_dir = Path(sys.argv[1]), Path(sys.argv[2])
    old = {e['asset']: e for e in json.loads((old_dir / 'catalog.json').read_text(encoding='utf-8-sig'))}
    new = {e['asset']: e for e in json.loads((new_dir / 'catalog.json').read_text(encoding='utf-8-sig'))}
    print(f'entries: reference {len(old)}, new {len(new)}; only in reference {len(old.keys() - new.keys())}, only in new {len(new.keys() - old.keys())}')
    for asset in sorted(old.keys() - new.keys())[:10]: print('  missing in new:', asset)
    for asset in sorted(new.keys() - old.keys())[:10]: print('  extra in new:', asset)
    fields = ('file', 'trick', 'pop', 'native_looping', 'position_codec')
    field_diffs = {f: 0 for f in fields + ('preview_only', 'category', 'label')}
    worst = dict(angle=(0, ''), position=(0, ''), scale=(0, ''), board=(0, ''))
    timing, marks, board_flag, failures = [], [], [], []
    for asset in sorted(old.keys() & new.keys()):
        a, b = old[asset], new[asset]
        for f in fields:
            if a.get(f) != b.get(f): field_diffs[f] += 1
        if bool(a.get('preview_only')) != bool(b.get('preview_only')): field_diffs['preview_only'] += 1
        if not a.get('preview_only'):
            for f in ('category', 'label'):
                if a.get(f) != b.get(f): field_diffs[f] += 1
        try:
            t0, f0, b0, up0, down0 = read_clip(old_dir / a['file'])
            t1, f1, b1, up1, down1 = read_clip(new_dir / b['file'])
        except Exception as error:  # noqa: BLE001
            failures.append((asset, str(error)))
            continue
        if t0.shape != t1.shape or not np.allclose(t0, t1, atol=1e-6):
            timing.append(asset)
            continue
        q0, q1 = f0[:, :, 3:7].astype(float), f1[:, :, 3:7].astype(float)
        q0 /= np.linalg.norm(q0, axis=2)[:, :, None]
        q1 /= np.linalg.norm(q1, axis=2)[:, :, None]
        angle = float(np.degrees(2 * np.arccos(np.clip(np.min(np.abs(np.sum(q0 * q1, axis=2))), 0, 1))))
        position = float(np.max(np.abs(f0[:, :, 7:] - f1[:, :, 7:])))
        scale = float(np.max(np.abs(f0[:, :, :3] - f1[:, :, :3])))
        for key, value in (('angle', angle), ('position', position), ('scale', scale)):
            if value > worst[key][0]: worst[key] = (value, asset)
        if (b0 is None) != (b1 is None): board_flag.append(asset)
        elif b0 is not None:
            d = float(np.max(np.abs(b0 - b1)))
            if d > worst['board'][0]: worst['board'] = (d, asset)
        if not a.get('preview_only'):
            marks.append((asset, round((up1 - up0) * 60), round((down1 - down0) * 60)))
    print('field differences:', field_diffs)
    print('timing differs:', len(timing), timing[:5])
    print('board flag differs:', len(board_flag), board_flag[:5])
    print('unreadable:', len(failures), failures[:5])
    print(f"worst rotation {worst['angle'][0]:.5f} deg ({worst['angle'][1]})")
    print(f"worst position {worst['position'][0]:.7f} m ({worst['position'][1]})")
    print(f"worst scale {worst['scale'][0]:.7f} ({worst['scale'][1]})")
    print(f"worst board {worst['board'][0]:.7f} ({worst['board'][1]})")
    off = [m for m in marks if m[1] or m[2]]
    print(f'trick marks: {len(marks)} tricks, {len(off)} with a pop or landing on another frame', off[:12])


if __name__ == '__main__':
    main()
