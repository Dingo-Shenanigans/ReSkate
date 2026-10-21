"""Bounded identical virtual-controller sequences for Streets and the C++ demo.

Dry-run by default. Execute only against one of the two fixed authorized EXEs,
held PID/creation identity and foreground window. Does not activate a window,
send OS keyboard input, install a driver, or load game code. Escape aborts.
"""
from __future__ import annotations
import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import time

_spec=importlib.util.spec_from_file_location('bmx_existing_controller_backend',Path(__file__).with_name('drive_demo.py'))
backend=importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(backend)

PACKAGE = Path(__file__).resolve().parents[1]
TARGETS = {'demo': PACKAGE/'build'/'bmx_demo.exe',
           'streets': Path(r'D:\SteamLibrary\steamapps\common\BMX Streets\BMX Streets.exe')}
A, B, LB, RB, R3, DPAD_DOWN = 0x1000, 0x2000, 0x0100, 0x0200, 0x0080, 0x0002
NEUTRAL = dict(buttons=0,lx=0,ly=0,rx=0,ry=0,lt=0,rt=0)
MATRIX_CYCLE_SECONDS = 3.6
MATRIX_DIRECTIONS = ((0,32767),(23170,23170),(32767,0),(23170,-23170),
                     (0,-32767),(-23170,-23170),(-32767,0),(-23170,23170))
MATRIX_MODIFIERS = ({'buttons':LB,'rt':255},{'buttons':RB,'lt':255},{'buttons':LB|RB},
                    {'buttons':LB},{'buttons':RB},{'lt':255,'rt':255},{'lt':255},{'rt':255})


def utc():
    return datetime.now(timezone.utc).isoformat()


def matrix_attempts(name):
    if name=='mc_griddle_right_retry':indices=[11]*3
    elif name=='visual_gap_retry':indices=[15,27,15,27]
    elif name=='trick_matrix_reset_fast':indices=range(64)
    elif name=='trick_matrix':indices=range(64)
    elif name in tuple('trick_matrix_'+suffix+f'{i:02}' for suffix in ('','reset_') for i in range(1,5)):
        first=(int(name[-2:])-1)*16;indices=range(first,first+16)
    else:return []
    attempts=[]
    for local,index in enumerate(indices):
        reset='_reset_' in name or name in ('mc_griddle_right_retry','visual_gap_retry')
        fast=name=='trick_matrix_reset_fast';cycle=4.5 if fast else 8. if reset else MATRIX_CYCLE_SECONDS
        start=round(local*cycle,3);set_index,slot=divmod(index,8);hop=.8 if fast else 1.5 if reset else .5
        attempts.append(dict(attempt_index=local,matrix_index=index,set=set_index,slot=slot,
            start_seconds=start,hop_request_seconds=round(start+hop,3),hop_release_seconds=round(start+hop+.1,3),
            modifier_start_seconds=round(start+hop+.16,3),selection_start_seconds=round(start+hop+.24,3),
            selection_end_seconds=round(start+hop+.64,3),end_seconds=round(start+cycle,3),
            reset_press_seconds=round(start+.05,3) if reset else None,reset_release_seconds=round(start+.15,3) if reset else None,
            requested_axes=dict(rx=MATRIX_DIRECTIONS[slot][0],ry=MATRIX_DIRECTIONS[slot][1]),
            requested_modifiers=MATRIX_MODIFIERS[set_index].copy()))
        if name=='mc_griddle_right_retry' and local==0:
            attempts[-1]['modifier_start_seconds']=round(start+1.60,3)
            attempts[-1]['selection_start_seconds']=round(start+1.64,3)
            attempts[-1]['selection_end_seconds']=round(start+2.04,3)
    return attempts


def matrix_timeline(name):
    timeline=[(0,NEUTRAL.copy())]
    for attempt in matrix_attempts(name):
        modifiers=NEUTRAL|attempt['requested_modifiers'];selection=modifiers|attempt['requested_axes']
        if attempt['reset_press_seconds'] is not None:
            timeline.extend([(attempt['reset_press_seconds'],NEUTRAL|dict(buttons=DPAD_DOWN)),
                             (attempt['reset_release_seconds'],NEUTRAL.copy())])
        timeline.extend([(attempt['hop_request_seconds'],NEUTRAL|dict(buttons=R3)),
                         (attempt['hop_release_seconds'],NEUTRAL.copy()),
                         (attempt['modifier_start_seconds'],modifiers),
                         (attempt['selection_start_seconds'],selection),
                         (attempt['selection_end_seconds'],NEUTRAL.copy())])
    timeline.append((matrix_attempts(name)[-1]['end_seconds'],NEUTRAL.copy()))
    return timeline


def scenarios():
    def state(**changes): return NEUTRAL | changes
    result={
        'confirm': [(0,state()),(.2,state(buttons=A)),(.35,state()),(1.5,state())],
        'reset_probe': [(0,state()),(.2,state(buttons=DPAD_DOWN)),(.35,state()),(2,state())],
        'turn90': [(0,state()),(.2,state(buttons=R3)),(.3,state()),(.4,state(lx=32767)),(.65,state()),(3,state())],
        'creep': [(0,state()),(.2,state(buttons=A)),(1,state()),(1.5,state())],
        'accel_brake': [(0,state()),(.5,state(buttons=A)),(4.5,state()),
                        (6,state(buttons=B)),(9,state()),(12,state())],
        'hop': [(0,state()),(1,state(buttons=R3)),(1.12,state()),(5,state())],
        'tricks': [(0,state()),(.5,state(buttons=A)),(3.8,state()),
                   (4,state(buttons=R3)),(4.1,state()),(4.22,state(buttons=LB,ry=-32767)),(4.55,state()),
                   (6.3,state(buttons=R3)),(6.4,state()),(6.52,state(lt=255,rt=255,rx=32767)),(6.9,state()),
                   (8.2,state(buttons=B)),(9.4,state()),(10,state())],
        'manual': [(0,state()),(.5,state(buttons=A)),(2.5,state()),
                   (3,state(ry=-13107)),(5,state()),(5.5,state(ry=13107)),(7.5,state()),(9,state())],
        'steer': [(0,state()),(.5,state(buttons=A)),(3,state(lx=9830)),
                  (5,state()),(6,state(buttons=B)),(8,state())],
        'full': [(0,state()),(.5,state(buttons=A)),(4.5,state()),
                 (5.2,state(buttons=B)),(8.5,state()),
                 (10,state(buttons=R3)),(10.1,state()),
                 (12,state(buttons=R3)),(12.1,state()),
                 (12.22,state(buttons=LB,ry=-32767)),(12.55,state()),
                 (15,state(buttons=R3)),(15.1,state()),
                 (15.22,state(lt=255,rt=255,rx=32767)),(15.6,state()),
                 (18,state(ry=-13107)),(19,state()),(30,state())],
        'flat_short': [(0,state()),(.5,state(buttons=A)),(2.5,state()),
                       (3,state(buttons=B)),(6,state()),(8,state(buttons=R3)),(8.1,state()),(12,state())],
        'suspension': [(0,state()),(.7,state(ry=-9830)),(1.2,state()),
                       (1.8,state(ry=-19660)),(2.2,state()),(2.8,state(ry=-32767)),
                       (3.2,state(ry=32767)),(3.3,state()),(6,state(buttons=R3)),(6.1,state()),
                       (9,state(ly=-16384)),(9.4,state(ly=16384)),(9.8,state()),
                       (11,state(buttons=R3)),(11.1,state()),(18,state())],
    }
    # Each attempt returns to the verified marker before another raw-axis
    # request. Names describe input, not an inferred native movement result.
    rotation=[(0,state())];controls=[(0,state())]
    for index,axis in enumerate((dict(lx=32767),dict(lx=-32767),dict(ly=32767),dict(ly=-32767))):
        start=8*index
        rotation.extend([(start+.05,state(buttons=DPAD_DOWN)),(start+.15,state()),
                         (start+1.5,state(buttons=R3)),(start+1.6,state()),
                         (start+1.74,state(**axis)),(start+2.14,state())])
    for index,value in enumerate((-13107,13107,-32767,32767)):
        start=8*index
        controls.extend([(start+.05,state(buttons=DPAD_DOWN)),(start+.15,state()),
                         (start+.5,state(buttons=A)),(start+1.8,state()),
                         (start+2,state(ry=value)),(start+3.2,state())])
    rotation.append((32,state()));controls.append((32,state()))
    result['air_rotation']=rotation;result['controls_probe']=controls
    for name in ('trick_matrix','mc_griddle_right_retry','visual_gap_retry','trick_matrix_reset_fast')+tuple('trick_matrix_'+suffix+f'{i:02}' for suffix in ('','reset_') for i in range(1,5)):
        result[name]=matrix_timeline(name)
    return result


class Guard:
    def __init__(self, pid, target):
        self.pid=pid; self.expected=TARGETS[target].resolve(strict=True); self.handle=None
        self.kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        self.user=ctypes.WinDLL('user32',use_last_error=True)
        k,u=self.kernel,self.user
        k.OpenProcess.argtypes=[wintypes.DWORD,wintypes.BOOL,wintypes.DWORD];k.OpenProcess.restype=wintypes.HANDLE
        k.CloseHandle.argtypes=[wintypes.HANDLE];k.CloseHandle.restype=wintypes.BOOL
        k.QueryFullProcessImageNameW.argtypes=[wintypes.HANDLE,wintypes.DWORD,wintypes.LPWSTR,ctypes.POINTER(wintypes.DWORD)]
        k.QueryFullProcessImageNameW.restype=wintypes.BOOL
        k.GetProcessTimes.argtypes=[wintypes.HANDLE]+[ctypes.POINTER(wintypes.FILETIME)]*4;k.GetProcessTimes.restype=wintypes.BOOL
        k.GetExitCodeProcess.argtypes=[wintypes.HANDLE,ctypes.POINTER(wintypes.DWORD)];k.GetExitCodeProcess.restype=wintypes.BOOL
        u.GetForegroundWindow.restype=wintypes.HWND
        u.GetWindowThreadProcessId.argtypes=[wintypes.HWND,ctypes.POINTER(wintypes.DWORD)];u.GetWindowThreadProcessId.restype=wintypes.DWORD
        u.GetAsyncKeyState.argtypes=[ctypes.c_int];u.GetAsyncKeyState.restype=ctypes.c_short
        self.handle=k.OpenProcess(0x1000,False,pid)
        if not self.handle: raise RuntimeError('Target process metadata unavailable')
        try:
            self.creation=None;self.token=self.file_token();self.sha256=hashlib.sha256(self.expected.read_bytes()).hexdigest()
            self.check()
        except BaseException:
            self.close();raise

    def file_token(self):
        s=self.expected.stat();return s.st_dev,s.st_ino,s.st_size,s.st_mtime_ns,s.st_ctime_ns

    def check(self):
        if self.user.GetAsyncKeyState(0x1b)&0x8000:raise RuntimeError('Escape abort')
        pid=wintypes.DWORD();window=self.user.GetForegroundWindow()
        self.user.GetWindowThreadProcessId(window,ctypes.byref(pid))
        if pid.value!=self.pid:raise RuntimeError('Selected target lost foreground')
        status=wintypes.DWORD()
        if not self.kernel.GetExitCodeProcess(self.handle,ctypes.byref(status)) or status.value!=259:raise RuntimeError('Target ended')
        name=ctypes.create_unicode_buffer(32768);n=wintypes.DWORD(32768)
        if not self.kernel.QueryFullProcessImageNameW(self.handle,0,name,ctypes.byref(n)) or os.path.normcase(name.value)!=os.path.normcase(str(self.expected)):
            raise RuntimeError('PID does not match the fixed selected executable')
        times=[wintypes.FILETIME() for _ in range(4)]
        if not self.kernel.GetProcessTimes(self.handle,*map(ctypes.byref,times)):raise RuntimeError('Creation time unavailable')
        creation=times[0].dwHighDateTime<<32|times[0].dwLowDateTime
        if self.creation is None:self.creation=creation
        if creation!=self.creation or self.file_token()!=self.token:raise RuntimeError('Target identity changed')

    def close(self):
        if self.handle:self.kernel.CloseHandle(self.handle);self.handle=None


def wait_for_foreground(pid,seconds):
    """Read-only preflight; no controller exists until Guard subsequently passes."""
    if seconds==0:return 0.0
    user=ctypes.WinDLL('user32',use_last_error=True)
    user.GetForegroundWindow.restype=wintypes.HWND
    user.GetWindowThreadProcessId.argtypes=[wintypes.HWND,ctypes.POINTER(wintypes.DWORD)]
    user.GetWindowThreadProcessId.restype=wintypes.DWORD
    user.GetAsyncKeyState.argtypes=[ctypes.c_int];user.GetAsyncKeyState.restype=ctypes.c_short
    started=time.monotonic()
    while True:
        if user.GetAsyncKeyState(0x1b)&0x8000:raise RuntimeError('Escape abort during foreground preflight')
        foreground_pid=wintypes.DWORD()
        user.GetWindowThreadProcessId(user.GetForegroundWindow(),ctypes.byref(foreground_pid))
        elapsed=time.monotonic()-started
        if foreground_pid.value==pid:return elapsed
        if elapsed>=seconds:raise RuntimeError('Foreground preflight timed out before controller creation')
        time.sleep(min(.05,seconds-elapsed))


def fresh_path(path):
    if path is None:return None
    path=path.resolve(strict=False)
    if not path.is_relative_to(PACKAGE) or path.exists():raise RuntimeError('Output/marker must be fresh and inside the package')
    path.parent.mkdir(parents=True,exist_ok=True)
    return path


def publish_start_marker(path,value):
    """Publish one complete marker; a polling observer never sees a writer lock."""
    temporary=path.with_name(path.name+'.writing.tmp')
    created=False
    try:
        with temporary.open('x',encoding='utf8') as target:
            created=True;json.dump(value,target,indent=2);target.flush();os.fsync(target.fileno())
        if path.exists():raise FileExistsError('Start marker appeared before atomic publication')
        os.replace(temporary,path);created=False
    finally:
        if created and temporary.exists():temporary.unlink()


def run(args):
    foreground_wait=getattr(args,'foreground_wait_seconds',0)
    if not 0<=foreground_wait<=30:raise RuntimeError('Foreground wait must be bounded to 0..30 seconds')
    if args.scenario in ('confirm','turn90','creep') and args.target!='streets':
        raise RuntimeError('Navigation scenarios are scoped to the installed Streets target')
    timeline=scenarios()[args.scenario]
    canonical=json.dumps(timeline,separators=(',',':'),sort_keys=True)
    timeline_sha=hashlib.sha256(canonical.encode()).hexdigest()
    if timeline[-1][0]>300:raise RuntimeError('Fixed controller timeline exceeds300seconds')
    attempts=matrix_attempts(args.scenario)
    if not args.execute:return dict(dry_run=True,target=args.target,executable=str(TARGETS[args.target]),
                                    driver_accessed=False,scenario=args.scenario,timeline_sha256=timeline_sha,
                                    scenario_duration_seconds=timeline[-1][0],matrix_attempts=attempts,
                                    timeline=[dict(at_seconds=t,**s) for t,s in timeline])
    if not args.pid or not args.output:raise RuntimeError('--execute requires --pid and --output')
    output=fresh_path(args.output);marker=fresh_path(args.start_marker)
    if marker==output:raise RuntimeError('Driver output and start marker must be distinct paths')
    arm_file=fresh_path(args.arm_file)
    connected=fresh_path(Path(str(output)+'.connected.json')) if arm_file else None
    if arm_file and (arm_file in (output,marker,connected) or connected==marker):
        raise RuntimeError('Driver, marker, connected proof and arm paths must be distinct')
    if args.ready_gate and not args.ready_gate.resolve().is_relative_to(PACKAGE):raise RuntimeError('Ready gate must be inside package')
    result=dict(schema=1,event='bmx_matched_virtual_controller',target=args.target,scenario=args.scenario,
                timeline_sha256=timeline_sha,started_utc=utc(),process_id=args.pid,executable=str(TARGETS[args.target]),
                completed=False,failure=None,reports=[],transitions=[],neutral_release_succeeded=False,
                disconnect_succeeded=False,driver_setup_changed=False,input_consumption_proven=False)
    result['scenario_duration_seconds']=timeline[-1][0]
    if attempts:result['matrix_attempts']=attempts
    guard=pad=None
    try:
        result['foreground_wait_limit_seconds']=foreground_wait
        result['foreground_wait_elapsed_seconds']=wait_for_foreground(args.pid,foreground_wait)
        guard=Guard(args.pid,args.target);result['executable_sha256']=guard.sha256;result['process_creation_filetime']=str(guard.creation)
        # Reuses the previously reviewed/pinned driver files. It validates only
        # that private backend; Guard separately validates the game executable.
        pad,module,client=backend.backend();result['device_created_utc']=utc()
        api=ctypes.WinDLL('xinput1_4.dll');api.XInputGetState.argtypes=[wintypes.DWORD,ctypes.POINTER(backend.PadState)];api.XInputGetState.restype=wintypes.DWORD
        slot=wintypes.DWORD(0xffffffff)
        if client.vigem_target_x360_get_user_index(pad._busp,pad._devicep,ctypes.byref(slot))!=0x20000000 or slot.value>3:raise RuntimeError('Virtual XInput slot unavailable')
        result['xinput_slot']=slot.value
        guard.check();pad.left_joystick(x_value=1,y_value=0);pad.update();time.sleep(.02);pad.reset();pad.update()
        for _ in range(50):
            guard.check();value=backend.PadState();error=api.XInputGetState(slot,ctypes.byref(value))
            if not error and bytes(value.report)==bytes(12):break
            time.sleep(.02)
        else:raise RuntimeError('Neutral bootstrap readback failed')
        result['neutral_bootstrap_proven']=True
        if arm_file:
            timeout=getattr(args,'arm_timeout',180);max_navigation=getattr(args,'max_navigation',3)
            navigation_file=fresh_path(Path(str(output)+'.navigation.json'))
            result['navigation_events']=[];seen_nonces=set();invalid_json_reads=0
            with navigation_file.open('x',encoding='utf8') as f:json.dump(dict(schema=1,navigation_events=[]),f)
            proof=dict(schema=1,event='bmx_virtual_controller_connected',target=args.target,process_id=args.pid,
                       executable=str(guard.expected),executable_sha256=guard.sha256,xinput_slot=slot.value,
                       neutral_bootstrap_proven=True,connected_utc=utc(),arm_file=str(arm_file),
                       arm_timeout_seconds=timeout,navigation_file=str(navigation_file),max_navigation=max_navigation,
                       allowed_navigation=['confirm','turn90','creep'],timeline_started=False)
            with connected.open('x',encoding='utf8') as f:json.dump(proof,f,indent=2)
            result['connected_proof']=str(connected);result['arm_wait_started_utc']=proof['connected_utc']
            deadline=time.perf_counter()+timeout;neutral_checks=0
            while True:
                guard.check();value=backend.PadState();error=api.XInputGetState(slot,ctypes.byref(value))
                if error or bytes(value.report)!=bytes(12):raise RuntimeError('Controller neutral readback lost while waiting for arm')
                neutral_checks+=1
                if arm_file.exists():
                    try:gate=json.loads(arm_file.read_text(encoding='utf-8-sig'))
                    except json.JSONDecodeError:gate=None;invalid_json_reads+=1
                    if gate is not None:
                        if not isinstance(gate,dict) or gate.get('target')!=args.target or gate.get('pid')!=args.pid:
                            raise RuntimeError('Arm file does not authorize this exact selected target/PID')
                        if gate.get('armed') is True:break
                        prelude=gate.get('prelude');nonce=gate.get('nonce')
                        if gate.get('armed') is not False or prelude not in ('confirm','turn90','creep') or not isinstance(nonce,str) or not 1<=len(nonce)<=64:
                            raise RuntimeError('Disarmed arm file requires one approved prelude and a nonce')
                        if nonce not in seen_nonces:
                            if len(seen_nonces)>=max_navigation:raise RuntimeError('Maximum approved navigation preludes reached')
                            seen_nonces.add(nonce);nav=scenarios()[prelude]
                            event=dict(prelude=prelude,nonce=nonce,started_utc=utc(),completed=False,reports=[])
                            result['navigation_events'].append(event)
                            nav_start=time.perf_counter();nav_index=0;nav_tick=nav_start
                            while time.perf_counter()-nav_start<=nav[-1][0]:
                                now=time.perf_counter();elapsed=now-nav_start
                                if now>deadline:raise RuntimeError('Arm-hold timeout during navigation')
                                if now-nav_tick>.25:raise RuntimeError('Navigation scheduler delayed over250ms')
                                nav_tick=now;guard.check()
                                while nav_index+1<len(nav) and elapsed>=nav[nav_index+1][0]:nav_index+=1
                                state=nav[nav_index][1];pad.reset()
                                for mask,name in [(A,'XUSB_GAMEPAD_A'),(R3,'XUSB_GAMEPAD_RIGHT_THUMB')]:
                                    if state['buttons']&mask:pad.press_button(getattr(module.XUSB_BUTTON,name))
                                pad.left_joystick(state['lx'],state['ly']);pad.right_joystick(state['rx'],state['ry']);pad.left_trigger(state['lt']);pad.right_trigger(state['rt']);pad.update()
                                value=backend.PadState();error=api.XInputGetState(slot,ctypes.byref(value))
                                readback={name:getattr(value.report,name) for name,_ in backend.PadReport._fields_}
                                matches=not error and readback==state
                                event['reports'].append(dict(at_seconds=elapsed,utc=utc(),requested=state.copy(),xinput_error=error,readback=readback,matches=matches))
                                if not matches:raise RuntimeError('Navigation driver readback did not match requested approved state')
                                time.sleep(.02)
                            pad.reset();pad.update();guard.check()
                            event['ended_utc']=utc();event['completed']=True
                            with navigation_file.open('w',encoding='utf8') as f:json.dump(dict(schema=1,navigation_events=result['navigation_events']),f,indent=2)
                if time.perf_counter()>deadline:raise RuntimeError('Arm file did not arrive within configured timeout')
                time.sleep(.05)
            result['arm_wait_ended_utc']=utc();result['arm_neutral_readback_count']=neutral_checks
            result['arm_incomplete_json_reads']=invalid_json_reads
            result['arm_file_sha256']=hashlib.sha256(arm_file.read_bytes()).hexdigest()
        if args.ready_gate:
            deadline=time.perf_counter()+60
            while True:
                guard.check()
                if args.ready_gate.exists():
                    gate=json.loads(args.ready_gate.read_text(encoding='utf-8-sig'))
                    if gate.get('ready') is True and gate.get('target')==args.target and gate.get('pid')==args.pid:break
                if time.perf_counter()>deadline:raise RuntimeError('Ready gate did not arrive within60s')
                time.sleep(.05)
        start=time.perf_counter();result['timeline_start_utc']=utc();result['timeline_start_perf_counter']=start
        if marker:
            publish_start_marker(marker,{k:v for k,v in result.items() if k not in ['reports','transitions']})
        i=0;last_index=-1;last_tick=start
        while time.perf_counter()-start<=timeline[-1][0]:
            now=time.perf_counter();elapsed=now-start
            if now-last_tick>.25:raise RuntimeError('Controller scheduler delayed over250ms')
            last_tick=now;guard.check()
            while i+1<len(timeline) and elapsed>=timeline[i+1][0]:i+=1
            state=timeline[i][1]
            if i!=last_index:
                result['transitions'].append(dict(event_index=i,requested_at_seconds=timeline[i][0],actual_at_seconds=elapsed,utc=utc(),state=state.copy()))
                last_index=i
            pad.reset()
            for mask,name in [(A,'XUSB_GAMEPAD_A'),(B,'XUSB_GAMEPAD_B'),(LB,'XUSB_GAMEPAD_LEFT_SHOULDER'),(RB,'XUSB_GAMEPAD_RIGHT_SHOULDER'),(R3,'XUSB_GAMEPAD_RIGHT_THUMB'),(DPAD_DOWN,'XUSB_GAMEPAD_DPAD_DOWN')]:
                if state['buttons']&mask:pad.press_button(getattr(module.XUSB_BUTTON,name))
            pad.left_joystick(state['lx'],state['ly']);pad.right_joystick(state['rx'],state['ry']);pad.left_trigger(state['lt']);pad.right_trigger(state['rt']);pad.update()
            value=backend.PadState();error=api.XInputGetState(slot,ctypes.byref(value))
            readback={name:getattr(value.report,name) for name,_ in backend.PadReport._fields_}
            result['reports'].append(dict(at_seconds=elapsed,utc=utc(),requested=state.copy(),xinput_error=error,readback=readback,
                                          matches=not error and readback==state))
            if error or readback!=state:raise RuntimeError('Driver readback did not match requested fixed timeline state')
            time.sleep(.02)
        result['completed']=True
    except BaseException as error:
        result['failure']=str(error)
    finally:
        if pad is not None:
            try:pad.reset();pad.update();result['neutral_release_succeeded']=True
            except Exception as error:result['neutral_release_failure']=str(error)
            try:pad.close();result['disconnect_succeeded']=True
            except Exception as error:result['disconnect_failure']=str(error)
            if 'xinput_slot' in result:
                for _ in range(25):
                    value=backend.PadState();error=api.XInputGetState(slot,ctypes.byref(value))
                    if error==1167:break
                    time.sleep(.02)
                result['post_disconnect_xinput_error']=error;result['disconnect_readback_proven']=error==1167
        if guard is not None:guard.close()
        result['ended_utc']=utc();result['driver_report_count']=len(result['reports'])
        result['matching_driver_reports']=sum(r['matches'] for r in result['reports'])
        with output.open('x',encoding='utf8') as f:json.dump(result,f,indent=2)
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target',choices=TARGETS,required=True);p.add_argument('--scenario',choices=scenarios(),default='accel_brake')
    p.add_argument('--execute',action='store_true');p.add_argument('--pid',type=int);p.add_argument('--output',type=Path)
    p.add_argument('--start-marker',type=Path);p.add_argument('--ready-gate',type=Path)
    p.add_argument('--arm-file',type=Path,help='Fresh JSON path: hold neutral up to180s until armed:true,target,pid matches')
    p.add_argument('--arm-timeout',type=int,choices=range(1,301),default=180,help='Explicit maximum neutral/navigation hold, at most300seconds')
    p.add_argument('--max-navigation',type=int,choices=range(4),default=3,help='Maximum approved confirm/turn90/creep nonce requests while held, at most3')
    p.add_argument('--foreground-wait-seconds',type=int,choices=range(31),default=0,help='Read-only preflight wait before target Guard and controller creation, at most 30 seconds')
    result=run(p.parse_args());print(json.dumps({k:v for k,v in result.items() if k not in ['reports']},indent=2))
    if result.get('failure'):raise SystemExit(1)
