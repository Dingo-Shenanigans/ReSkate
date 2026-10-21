"""Generate opt-in timing parameters from the current live bound clip catalog.

ApplySpeed's MasterSpeed * OverallSpeedMult * phase multiplier consumer has
separate native evidence. These are intrinsic clip durations, not proof of a
completed or landed trick. Raw clips and game assets are not copied.
"""
from pathlib import Path
import hashlib
import json

PACKAGE=Path(__file__).resolve().parents[1]


def main():
    source=PACKAGE/'reference/current-trick-catalog-timing-v3.json'
    data=json.loads(source.read_text(encoding='utf-8-sig'))
    entries=[]
    for row in data['catalog_slots']:
        if not row.get('name'):continue
        speed=row.get('raw_speed_getters') or {}
        clips=row.get('clips') or {}
        def duration(phase):
            lengths=[clip['length_seconds'] for name,clip in clips.items()
                     if clip and name in (f'Player{phase}Clip',f'Vehicle{phase}Clip')]
            overall=speed.get('OverallSpeedMult',0);factor=speed.get(f'{phase}SpeedMult',0)
            return max(lengths)/(overall*factor) if lengths and overall>0 and factor>0 else 0
        index=row['set']*10+row['slot'];entries.append((index,row['name'],duration('Enter'),duration('Exit')))
    lines=['#pragma once','#include "movement.hpp"','#include <cmath>','',
           'namespace bmx {',
           '// Intrinsic durations from current bound clips and their native speed consumer.',
           '// MasterSpeed is the selected progression OR character multiplier, not their product.',
           '// Controller transition, hold, cancel and scoring policies remain independent.',
           'inline void apply_streets_observed_trick_timing(TrickSettings& settings,double master_speed=1) noexcept {',
           '    if(!std::isfinite(master_speed)||master_speed<=0)return;']
    for index,name,enter,exit in entries:
        lines.append(f'    settings.enter_seconds[{index}]={enter:.12g}/master_speed;settings.recover_seconds[{index}]={exit:.12g}/master_speed; // {name}')
    lines.extend(['    settings.bar_seconds=(settings.enter_seconds[34]+settings.enter_seconds[44])*.5;',
                  '    settings.tail_seconds=(settings.enter_seconds[52]+settings.enter_seconds[56])*.5;','}','}'])
    out=PACKAGE/'include/bmx/observed_trick_timing.hpp'
    out.write_text('\n'.join(lines)+'\n',encoding='utf8')
    receipt=dict(schema=1,source=str(source.relative_to(PACKAGE)),source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                 generated=str(out.relative_to(PACKAGE)),generated_sha256=hashlib.sha256(out.read_bytes()).hexdigest(),
                 entries=len(entries),master_speed=1,limits='Intrinsic clip duration only; actual controller transitions and scoring are independently observed.')
    (PACKAGE/'reference/observed-trick-timing.provenance.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf8')
    print(json.dumps(receipt,indent=2))


if __name__=='__main__':main()
