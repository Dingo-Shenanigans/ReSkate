"""Export a deliberately sparse F8 reference from a diagnostic dump, never a native asset.

Only complete observed ground/air/ground episodes are accepted. Board motion comes
from joint 380 of the same body publication, whose correspondence with the physical
mount is checked in the diagnostics. This does not recover missing 60 Hz samples,
establish active native clip/pop identity, or produce an animation replacement mod.
"""
import argparse
import csv
import json
from pathlib import Path
import struct

import numpy as np

def transform(row):
    x, y, z, w = [float(row[k]) for k in ('qx', 'qy', 'qz', 'qw')]
    q = np.array((x, y, z, w)); q /= np.linalg.norm(q)
    if not np.isfinite(q).all():
        raise ValueError('Invalid captured rotation')
    x, y, z, w = q
    m = np.eye(4)
    m[:3, :3] = np.array([
        [1-2*(y*y+z*z), 2*(x*y-z*w), 2*(x*z+y*w)],
        [2*(x*y+z*w), 1-2*(x*x+z*z), 2*(y*z-x*w)],
        [2*(x*z-y*w), 2*(y*z+x*w), 1-2*(x*x+y*y)],
    ]) @ np.diag([float(row[k]) for k in ('sx', 'sy', 'sz')])
    m[:3, 3] = [float(row[k]) for k in ('px', 'py', 'pz')]
    if not np.isfinite(m).all():
        raise ValueError('Invalid captured transform')
    return m


def export(dump, destination):
    timeline = list(csv.DictReader((dump / 'timeline.csv').open()))
    parents = {int(r['joint']): int(r['parent'])
               for r in csv.DictReader((dump / 'skeleton.csv').open())}
    if len(parents) != 395 or parents[380] != 1 or parents[390] != 380:
        raise ValueError('A verified 395-joint animation hierarchy is required')
    episodes = {}
    for row in timeline:
        if row['physics'] in ('200', '201'):
            episodes.setdefault(row['episode'], []).append(row)
    choices = []
    for episode, air in episodes.items():
        up = float(air[0]['seconds'])
        down_rows = [r for r in timeline if r['episode'] == episode and
                     r['physics'] == '100' and float(r['seconds']) > up]
        before = [r for r in timeline if float(r['seconds']) < up]
        names = {r['trick'] for r in air if r['trick']}
        if before and before[-1]['physics'] in ('100', '103') and down_rows and len(names) == 1:
            choices.append((up, float(down_rows[0]['seconds']), episode, names.pop(), air[0]))
    if not choices:
        raise ValueError('No complete named ground/air/ground episode is present')
    up, down, episode, trick, takeoff = choices[-1]
    poses = {}
    for row in csv.DictReader((dump / 'poses.csv').open()):
        if row['stage'] != 'render_before_override':
            continue
        t = float(row['seconds'])
        if up - .5 <= t <= down + .5:
            record = poses.setdefault(int(row['sequence']), (t, {}))
            record[1][int(row['joint'])] = row
    frames = sorted(poses.items(), key=lambda item: item[1][0])
    if len(frames) < 8 or any(len(pose) != 395 for _, (_, pose) in frames):
        raise ValueError('Incomplete render poses')
    times = [record[0] for _, record in frames]
    if times[0] >= up or times[-1] <= down:
        raise ValueError('The episode lacks pre-pop or post-landing render poses')
    if any(b <= a or b-a > .15 for a, b in zip(times, times[1:])):
        raise ValueError('Render history contains duplicate samples or a capture gap over 150 ms')
    origin = times[0]; relative_times = [t-origin for t in times]
    boards, body = [], []
    physical = {int(row['sequence']): transform(row) for row in csv.DictReader((dump / 'board.csv').open())
                if row['stage'] == 'render_before_override'}
    max_position_error = max_rotation_error = 0
    for seq, (_, pose) in frames:
        if seq not in physical:
            raise ValueError('A physical board sample is required to verify every captured board frame')
        skeleton_board = transform(pose[0]) @ transform(pose[1]) @ transform(pose[380])
        error = np.linalg.inv(physical[seq]) @ skeleton_board
        position_error = float(np.linalg.norm(error[:3, 3]))
        rotation_error = float(np.degrees(np.arccos(np.clip((np.trace(error[:3, :3])-1)/2, -1, 1))))
        if position_error > .03 or rotation_error > 2:
            raise ValueError('The captured skeleton board does not agree with the physical mount')
        max_position_error = max(max_position_error, position_error)
        max_rotation_error = max(max_rotation_error, rotation_error)
        # Joint 380 is directly under placement (1). Its exact local transform is
        # already the captured mount relative to placement: no separate board tick.
        boards.append(tuple(float(pose[380][k]) for k in ('sx','sy','sz','qx','qy','qz','qw','px','py','pz')))
        body.extend(tuple(float(pose[j][k]) for k in ('sx','sy','sz','qx','qy','qz','qw','px','py','pz')) for j in range(395))
    takeoff_time, landing_time = up-origin, down-origin
    payload = bytearray(struct.pack('<4sIIff', b'FAS2', 395, len(frames), takeoff_time, landing_time))
    payload.extend(struct.pack(f'<{len(frames)}f', *relative_times))
    for joint in body + boards:
        payload.extend(struct.pack('<10f', *joint))
    name = '360 Flip native reference' if trick == '360flip' else 'Native trick reference'
    report = {
        'dump': dump.name, 'episode': episode, 'observed_trick': trick,
        'sample_count': len(frames), 'max_sample_gap_seconds': max(np.diff(times)),
        'duration': relative_times[-1], 'takeoff': takeoff_time, 'landing': landing_time,
        'scope': 'Sparse diagnostic reference; no recovered intermediate frames, verified pop or native source asset identity',
        'board_source': 'Same body publication, animation joint 380; no independent physical-board timestamp',
        'max_physical_mount_position_error_metres': max_position_error,
        'max_physical_mount_rotation_error_degrees': max_rotation_error,
    }
    profile = {
        'format': 'fourth-animation-studio', 'version': 1, 'name': name,
        'trick': '', 'pop': '', 'stance': 'goofy' if takeoff['goofy'] == '1' else 'regular',
        'switch_stance': takeoff['live_switch'] == '1', 'entry': 'ground',
        'source': {'kind': 'render_capture', 'asset': '',
                   'rig': 'animation/dingo/animbase_default_skeleton', 'board_frame': 'skater-placement'},
        'duration': relative_times[-1], 'takeoff': takeoff_time, 'landing': landing_time,
        'speed': 1, 'loop': True, 'start': 0, 'end': -1, 'tracks': [],
        'diagnostic_reference': report,
    }
    destination.mkdir(parents=True, exist_ok=True)
    (destination / (name + '.clip')).write_bytes(payload)
    (destination / (name + '.json')).write_text(json.dumps(profile, indent=2))
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('dump', type=Path)
    parser.add_argument('destination', type=Path)
    args = parser.parse_args()
    print(json.dumps(export(args.dump, args.destination), indent=2))
