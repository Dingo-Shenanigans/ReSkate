"""Assemble a provenance/role ledger from completed actual comparison artifacts."""
from __future__ import annotations
import argparse
import csv
import hashlib
import importlib.util
import json
from pathlib import Path
import re

spec=importlib.util.spec_from_file_location('session_audit',Path(__file__).with_name('comparison.py'))
c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
PAIRS={'fast':'streets-visual-matrix01','air':'streets-air-v33','visual-retry':'streets-visual-retry-v33',
       'suspension':'streets-suspension-v33','controls':'streets-controls-v33'}


def artifact(path):
    with path.open('rb') as stream:digest=hashlib.file_digest(stream,'sha256').hexdigest()
    return dict(path=str(path),sha256=digest)
def read(path):return json.loads(path.read_text(encoding='utf-8-sig'))


def main(args):
    if not re.fullmatch(r'v2[a-z]',args.tag):raise ValueError('Expected explicit v2 revision tag')
    session=args.session.resolve(strict=True);module=Path(__file__).resolve().parents[1];runs=[];exe_hashes=set()
    for name,native_name in PAIRS.items():
        native=session/native_name;demo=session/f'demo-{name}-{args.tag}';comparison=session/f'comparison-{name}-{args.tag}'
        sp=session/(native_name+'.driver.json');dp=session/(demo.name+'.driver.json')
        sd=c.read_driver(sp,'streets');dd=c.read_driver(dp,'demo');video=read(comparison/'comparison-video.validation.json');report=read(comparison/'comparison.json')
        if sd['timeline_sha256']!=dd['timeline_sha256'] or report['timeline_sha256']!=dd['timeline_sha256']:raise ValueError('Paired timeline identity mismatch')
        if video.get('container_and_frame_bytes_verified') is not True or video.get('generated_motion') is not False:raise ValueError('Unverified/nonactual comparison video')
        if artifact(Path(video['path']))['sha256']!=video['sha256']:raise ValueError('Comparison video changed after byte validation')
        if report['source_driver_hashes']!=dict(streets=c.sha(sp.read_bytes()),demo=c.sha(dp.read_bytes())):raise ValueError('Comparison driver receipt changed')
        ds=read(demo/'capture-summary.json');ns=read(native/'capture-summary.json')
        if ds.get('completed') is not True or ds.get('scripted') is not False or ds.get('invalid_ticks')!=0:raise ValueError('Actual valid completed module capture required')
        if ds.get('hop_speed')!=6.4:raise ValueError('Diagnostic hop override must not be presented as production')
        with (session/(demo.name+'.csv')).open(newline='',encoding='utf-8-sig') as stream:
            rows=list(csv.DictReader(stream))
        invalid=sum(c.boolean(row.get('valid')) is False for row in rows)
        if invalid:raise ValueError('Invalid module body samples')
        coupled_path=demo/'coupled-physics.jsonl'
        coupled=[json.loads(line) for line in coupled_path.open(encoding='utf-8') if line.strip()]
        invalid_coupled=sum(row.get('valid') is not True for row in coupled)
        nonconverged_coupled=sum(row.get('converged') is not True for row in coupled)
        if len(coupled)!=len(rows) or invalid_coupled or nonconverged_coupled:
            raise ValueError('Complete valid and converged coupled samples required')
        binary=dd.get('executable_sha256')
        if not binary:raise ValueError('Driver must pin module executable')
        exe_hashes.add(binary)
        role=('actual_frame_and_pose_comparison_only; native rich fixed-physics/trick channels failed to open'
            if name=='fast' else 'actual_frame_and_recorded_body_response_comparison')
        runs.append(dict(name=name,role=role,scenario=dd['scenario'],timeline_sha256=dd['timeline_sha256'],
            native=dict(directory=str(native),capture=artifact(native/'capture-summary.json'),
                driver=artifact(sp),driver_reports=len(sd['reports']),fixed_physics_comparison_available=name!='fast',
                frames=ns.get('frames'),bone_samples=ns.get('bone_samples'),source_errors=ns.get('errors'),
                note='Counters do not restore the missing fixed-physics channel; use the explicit channel-limitation audit.' if name=='fast' else None),
            module=dict(directory=str(demo),capture=artifact(demo/'capture-summary.json'),driver=artifact(dp),
                body_csv=artifact(session/(demo.name+'.csv')),coupled_physics=artifact(coupled_path),
                coupled_samples=len(coupled),invalid_coupled_samples=invalid_coupled,nonconverged_coupled_samples=nonconverged_coupled,driver_reports=len(dd['reports']),body_samples=len(rows),invalid_body_samples=invalid,
                frames=ds['frames'],rider_pose_samples=ds['rider_pose_samples'],executable_sha256=binary,
                hop_speed=ds['hop_speed'],neutral_cleanup_verified=True,post_disconnect_error=dd['post_disconnect_xinput_error']),
            comparison=dict(report=artifact(comparison/'comparison.json'),video_audit=artifact(comparison/'comparison-video.validation.json'),
                video_sha256=video['sha256'],video_bytes=video['bytes'],encoded_frames=video['encoded_frames'],
                aligned_wall_interval=video['aligned_time_range_seconds'],short_gif=video.get('gif'),contact_sheet=video.get('contact_sheet'))))
    if len(exe_hashes)!=1:raise ValueError('Final module runs use different executables')
    current_exe=module/'build/bmx_demo.exe'
    if c.sha(current_exe.read_bytes())!=next(iter(exe_hashes)):raise ValueError('Current module executable differs from capture receipts')
    headless_path=args.headless_manifest.resolve(strict=True) if args.headless_manifest else session/'headless-release-v2d-manifest.json'
    headless=read(headless_path)
    if artifact(module/'build/libbmx_movement.a')['sha256']!=headless['library']['sha256']:
        raise ValueError('Current library differs from the frozen numerical validation')
    preflights=[];aborted_sessions=[]
    for receipt in sorted(session.glob('demo-fast-v2*.driver.json')):
        if not re.fullmatch(r'demo-fast-v2[a-z]\.driver\.json',receipt.name):continue
        rejected=read(receipt)
        if rejected.get('completed') is not False:continue
        record=dict(driver=artifact(receipt),reason=rejected.get('failure'),
            controller_created=bool(rejected.get('device_created_utc')),reports=rejected.get('driver_report_count',0),
            matching_reports=rejected.get('matching_driver_reports',0),
            neutral_release_succeeded=rejected.get('neutral_release_succeeded'),
            disconnect_readback_proven=rejected.get('disconnect_readback_proven'))
        (preflights if not record['controller_created'] and not record['reports'] else aborted_sessions).append(record)
    artifacts=[module/'reference'/name for name in ('observed-pose-tracks.provenance.json','observed-joint-actuation-tracks.provenance.json','observed-vehicle-ik-tracks.provenance.json')]
    artifacts += [headless_path]
    artifacts += [session/(name+args.tag+'.json') for name in ('coverage-demo-fast-','actual-render-channel-audit-fast-','live-posture-fast-','live-vehicle-ik-fast-','live-controls-','live-suspension-','live-air-')]
    result=dict(schema=2,event='actual_controller_comparison_session_role_manifest',final_revision=args.tag,module_version='0.3.0',
        module_executable=artifact(current_exe),library_at_manifest_generation=artifact(module/'build/libbmx_movement.a'),final_actual_runs=runs,
        source_fit_roles=dict(primary_trick_posture_and_physical_parts=['streets-reset-matrix01b','streets-reset-matrix02','streets-reset-matrix03','streets-reset-matrix04','streets-mc-griddle-retry01'],
            independent_posture_repeat_and_additional_vehicle_ik=['streets-visual-matrix01','streets-visual-retry-v33'],
            current_native_controls=['streets-controls-v33','streets-air-v33','streets-suspension-v33'],
            headless_engine_schedule_identification='analysis-suspension01/measured-drive-fixture.csv'),
        derived_artifacts=[artifact(path) for path in artifacts],
        historical_module_roles=dict(pre_flick_baseline=['demo-fast-v2g','demo-air-v2g','demo-visual-retry-v2g','demo-suspension-v2g','demo-controls-v2g'],pre_manual_pid=['demo-controls-v2','demo-fast-v2'],pre_source_air_drag=['demo-controls-v2b','demo-fast-v2b','demo-visual-retry-v2b','demo-suspension-v2b','demo-air-v2b']),
        rejected_controller_preflights=preflights,rejected_partial_controller_sessions=aborted_sessions,
        rejected_actual_runs=([dict(directory=str(session/'demo-fast-v2c'),reason='226 invalid ticks after the Mc Griddle Left attempt; discrete flat contact tunneling',
            audit=artifact(session/'analysis-demo-fast-v2c-invalid.json'))] if (session/'analysis-demo-fast-v2c-invalid.json').exists() else []),
        rejected_diagnostics=['headless-hop65-diagnostic','headless-hop68-diagnostic','headless-hop71-diagnostic','headless-hop72-diagnostic'],
        limits=['Native rich fixed physics/trick-state channels are unavailable in the fast visual source; its valid pose/IK/video channels are used explicitly.',
            'Nothing and Bus Driver Left require the separate native visual retry; the original fast-video failures are preserved.',
            'Native animation Idle is distinct from authoritative landed scoring, which was not observed.',
            'Module posture/vehicle reconstruction checks and independent native-repeat errors are different measurement domains.',
            'Reduced physical bodies, wheel articulation, flat-host rig collision response, camera and geometry differ.',
            'Actual per-wheel native grounded flags and module preintegration queries have distinct callback/solver semantics.',
            'Native engine/wall pacing, stationary camera-orbit input routing and inferred Manny movement mode limit attribution.',
            'Headless fixtures and invalid higher-hop diagnostics are not actual module controller recordings or release profiles.',
            'Game-install restoration is independently audited by the parent release verifier; this comparison manifest does not assert that filesystem check.'])
    with args.output.open('x',encoding='utf-8') as stream:json.dump(result,stream,indent=2)
    print(json.dumps(dict(output=str(args.output),final_runs=len(runs),driver_reports=sum(row['module']['driver_reports'] for row in runs),executable_sha256=next(iter(exe_hashes))),indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--session',type=Path,required=True);parser.add_argument('--tag',required=True)
    parser.add_argument('--headless-manifest',type=Path,help='Exact frozen numerical manifest; independent of the actual-capture suffix')
    parser.add_argument('--output',type=Path,required=True);main(parser.parse_args())
