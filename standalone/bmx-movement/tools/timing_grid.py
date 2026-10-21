"""Exercise bounded fixed-step timing variants; never connects a controller."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
from pathlib import Path
import subprocess
import time


def artifact(path: Path) -> dict:
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    return {'path': str(path.resolve()), 'sha256': digest}


def main(args) -> int:
    exe = args.exe.resolve(strict=True)
    library = args.library.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(exist_ok=False)
    started = time.monotonic()
    results = []
    priority = [(.22, .41), (.21, .40), (.23, .42), (.24, .40)]
    cases = priority + [(d, h) for d in [.21, .22, .23, .24, .25]
                        for h in [.39, .40, .41, .42] if (d, h) not in priority]
    sources = [artifact(exe), artifact(library), artifact(Path(__file__).with_name('sweep_coupled_tricks.cpp'))]

    def run(case):
        delay, hold = case
        destination = output / f'delay-{delay:.2f}-hold-{hold:.2f}'
        completed = subprocess.run(
            [str(exe), str(destination), '--hop-at', '.77',
             '--selection-delay', str(delay), '--selection-hold', str(hold)],
            capture_output=True, text=True, timeout=180, check=False)
        summary_path = destination / 'summary.json'
        data = json.loads(summary_path.read_text(encoding='utf-8'))
        counts = data['counts']
        counts['nonconverged_samples'] = sum(a['nonconverged_samples'] for a in data['attempts'])
        return dict(delay=delay, hold=hold, exit_code=completed.returncode, counts=counts,
                    failures=[a for a in data['attempts']
                              if a['invalid_samples'] or a['nonconverged_samples']],
                    summary_path=str(summary_path))

    def save():
        report = dict(schema=1, event='headless_fixed_step_input_timing_grid',
                      actual_controller_recording=False, completed_cases=len(results), expected_cases=20,
                      hop_at_seconds=.77, sources=sources,
                      slot_attempts=sum(r['counts']['attempted'] for r in results),
                      invalid_samples=sum(r['counts']['invalid_samples'] for r in results),
                      nonconverged_samples=sum(r['counts']['nonconverged_samples'] for r in results),
                      results=sorted(results, key=lambda r: (r['delay'], r['hold'])),
                      elapsed_seconds=time.monotonic() - started,
                      limits=['Fixed-step timing stress is separate from actual controller/video validation.',
                              'Source hop setting and production caps/guards remain unchanged.',
                              'Executable and current library hashes are recorded; link provenance must be retained by the build.'])
        (output / 'grid-summary.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        return report

    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for future in as_completed([pool.submit(run, case) for case in cases]):
            result = future.result()
            results.append(result)
            save()
            print(json.dumps({k: result[k] for k in ('delay', 'hold', 'counts')}), flush=True)
    report = save()
    print(json.dumps({k: report[k] for k in ('completed_cases', 'slot_attempts', 'invalid_samples',
                                          'nonconverged_samples', 'elapsed_seconds')}), flush=True)
    return 2 if report['invalid_samples'] or report['nonconverged_samples'] else 0


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--library', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--workers', type=int, choices=range(1, 5), default=4)
    raise SystemExit(main(parser.parse_args()))
