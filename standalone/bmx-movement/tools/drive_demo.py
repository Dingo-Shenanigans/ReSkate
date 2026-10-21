"""Bounded virtual Xbox controller rehearsal for the standalone bmx_demo only.

Default is dry-run. --execute requires the pinned preinstalled vgamepad package,
a running foreground demo PID, and the exact standalone/build/bmx_demo.exe.
No game launch, window activation, driver installation, or OS keyboard input.
"""
from __future__ import annotations
import argparse
import ctypes
from ctypes import wintypes
from datetime import datetime, timezone
import hashlib
import importlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import time

PACKAGE=Path(__file__).resolve().parents[1]
WORKSPACE=PACKAGE.parents[1]
EXE=PACKAGE/'build'/'bmx_demo.exe'
DEPENDENCY=WORKSPACE/'build'/'bmx-gamepad-runtime'
RECEIPT=WORKSPACE/'build'/'bmx'/'gamepad-setup'/'dependency-verification.json'
FILES={
 '__init__.py':('77EB34B1D35C255DFF78D1206E1C61400B12048AABDDD9DA5FA27FB350848D11',334),
 'win/__init__.py':('E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855',0),
 'win/vigem_client.py':('9598A035E44740715B11B1BA3FABA29B7FD1753BCA73DFA1AEC0EA514D9B02DB',10619),
 'win/vigem_commons.py':('BE76CF56FC5D1E07E070C76FE0D0BD846603C89387EC8FF3879713977A68829C',6470),
 'win/virtual_gamepad.py':('AA2211E4A17DCC34244DC5767D79EC61A285E8D47CF744BED1015E9F46CEEE53',13590),
 'win/vigem/client/x64/ViGEmClient.dll':('2BF0CB1D809039573C922737D298A1653D4DBC61408060FF45A9BCFDE82E97D2',130048)}
A,B,LB,R3=0x1000,0x2000,0x0100,0x0080
NEUTRAL=dict(buttons=0,lx=0,ly=0,rx=0,ry=0,lt=0,rt=0)


def timeline():
    def state(**changes):return NEUTRAL|changes
    return [(0,state()),(.5,state(buttons=A)),(3.8,state()),
            (4,state(buttons=R3)),(4.1,state()),(4.22,state(buttons=LB,ry=-32767)),(4.55,state()),
            (6.3,state(buttons=R3)),(6.4,state()),(6.52,state(lt=255,rt=255,rx=32767)),(6.9,state()),
            (8.2,state(buttons=B)),(9.4,state()),(10,state())]


class Guard:
    def __init__(self,pid):
        self.pid=pid;self.handle=None
        self.kernel=ctypes.WinDLL('kernel32',use_last_error=True)
        self.user=ctypes.WinDLL('user32',use_last_error=True)
        k=self.kernel;u=self.user
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
        if not self.handle:raise RuntimeError('Demo process metadata unavailable')
        try:
            self.creation=None;self.token=self.file_token();self.sha256=hashlib.sha256(EXE.read_bytes()).hexdigest()
            self.check()
        except BaseException:
            self.close();raise

    def file_token(self):
        s=EXE.stat();return (s.st_dev,s.st_ino,s.st_size,s.st_mtime_ns,s.st_ctime_ns)

    def check(self):
        if self.user.GetAsyncKeyState(0x1b)&0x8000:raise RuntimeError('Escape abort')
        pid=wintypes.DWORD();window=self.user.GetForegroundWindow()
        self.user.GetWindowThreadProcessId(window,ctypes.byref(pid))
        if pid.value!=self.pid:raise RuntimeError('Standalone demo lost foreground')
        status=wintypes.DWORD()
        if not self.kernel.GetExitCodeProcess(self.handle,ctypes.byref(status)) or status.value!=259:raise RuntimeError('Demo ended')
        name=ctypes.create_unicode_buffer(32768);n=wintypes.DWORD(32768)
        if not self.kernel.QueryFullProcessImageNameW(self.handle,0,name,ctypes.byref(n)) or os.path.normcase(name.value)!=os.path.normcase(str(EXE)):
            raise RuntimeError('PID is not the exact standalone demo executable')
        times=[wintypes.FILETIME() for _ in range(4)]
        if not self.kernel.GetProcessTimes(self.handle,*map(ctypes.byref,times)):raise RuntimeError('Creation time unavailable')
        creation=times[0].dwHighDateTime<<32|times[0].dwLowDateTime
        if self.creation is None:self.creation=creation
        if creation!=self.creation or self.file_token()!=self.token:raise RuntimeError('Demo identity changed')

    def close(self):
        if self.handle:self.kernel.CloseHandle(self.handle);self.handle=None


class PadReport(ctypes.Structure):
    _fields_=[('buttons',ctypes.c_ushort),('lt',ctypes.c_ubyte),('rt',ctypes.c_ubyte),('lx',ctypes.c_short),('ly',ctypes.c_short),('rx',ctypes.c_short),('ry',ctypes.c_short)]
class PadState(ctypes.Structure):
    _fields_=[('packet',wintypes.DWORD),('report',PadReport)]


def backend():
    if os.name!='nt':raise RuntimeError('Windows only')
    expected_python=DEPENDENCY/'Scripts'/'python.exe';package=DEPENDENCY/'Lib'/'site-packages'/'vgamepad'
    if Path(sys.executable).resolve()!=expected_python.resolve():raise RuntimeError('Use existing private bmx-gamepad-runtime/Scripts/python.exe')
    receipt=json.loads(RECEIPT.read_text(encoding='utf-8-sig'))
    if (receipt.get('schema')!=1 or receipt.get('event')!='bmx_virtual_gamepad_dependency' or receipt.get('version')!='0.1.0' or
        receipt.get('source_sha256')!='57F6BD01AEC0C172947517FB782D150EF9B285F7F4D524C317374FA5C24A89DE' or
        receipt.get('setup_py_executed') is not False or receipt.get('bundled_driver_installer_executed') is not False or
        Path(receipt.get('package_root','')).resolve()!=package.resolve()):raise RuntimeError('Dependency provenance mismatch')
    for path in [EXE,package]:
        while path!=WORKSPACE:
            if path.is_symlink() or (hasattr(path,'is_junction') and path.is_junction()):raise RuntimeError('Reparse paths refused')
            if path==path.parent:raise RuntimeError('Dependency path escaped workspace')
            path=path.parent
    actual={p.relative_to(package).as_posix() for p in package.rglob('*') if p.is_file()}
    if actual!=set(FILES):raise RuntimeError('Unexpected vgamepad package files')
    for relative,(sha,size) in FILES.items():
        data=(package/relative).read_bytes()
        if len(data)!=size or hashlib.sha256(data).hexdigest().upper()!=sha:raise RuntimeError('Unreviewed backend bytes: '+relative)
    if any(n=='vgamepad' or n.startswith('vgamepad.') for n in sys.modules):raise RuntimeError('Use fresh Python process')
    spec=importlib.util.find_spec('vgamepad')
    if spec is None or Path(spec.origin).resolve()!=(package/'__init__.py').resolve():raise RuntimeError('Unexpected package origin')
    sys.dont_write_bytecode=True
    module=importlib.import_module('vgamepad')
    class ManagedPad(module.VX360Gamepad):
        def __init__(self):self.closed=False;super().__init__()
        def close(self):
            if not self.closed:self.closed=True;super().__del__()
        def __del__(self):
            if getattr(self,'_devicep',None) and getattr(self,'_busp',None):self.close()
    return ManagedPad(),module,importlib.import_module('vgamepad.win.vigem_client')


def run(args):
    if not args.execute:return {'dry_run':True,'driver_accessed':False,'timeline':[dict(at_seconds=t,**s) for t,s in timeline()]}
    if not args.pid or not args.output:raise RuntimeError('--execute requires --pid and a fresh --output JSON file')
    output=args.output.resolve()
    if not output.is_relative_to(PACKAGE) or output.exists():raise RuntimeError('Output must be fresh and inside standalone package')
    output.parent.mkdir(parents=True,exist_ok=True)
    result=dict(schema=1,event='bmx_standalone_virtual_controller',started_utc=datetime.now(timezone.utc).isoformat(),
                process_id=args.pid,executable=str(EXE),completed=False,failure=None,reports=[],neutral_release_succeeded=False,
                disconnect_succeeded=False,driver_setup_changed=False,input_consumption_proven=False)
    guard=pad=None
    try:
        guard=Guard(args.pid);result['executable_sha256']=guard.sha256;result['process_creation_filetime']=str(guard.creation)
        pad,module,client=backend();result['device_created_utc']=datetime.now(timezone.utc).isoformat()
        api=ctypes.WinDLL('xinput1_4.dll');api.XInputGetState.argtypes=[wintypes.DWORD,ctypes.POINTER(PadState)];api.XInputGetState.restype=wintypes.DWORD
        slot=wintypes.DWORD(0xffffffff)
        if client.vigem_target_x360_get_user_index(pad._busp,pad._devicep,ctypes.byref(slot))!=0x20000000 or slot.value>3:raise RuntimeError('Virtual XInput slot unavailable')
        result['xinput_slot']=slot.value
        guard.check();pad.left_joystick(x_value=1,y_value=0);pad.update();time.sleep(.02);pad.reset();pad.update()
        for _ in range(50):
            guard.check();value=PadState();error=api.XInputGetState(slot,ctypes.byref(value))
            if not error and bytes(value.report)==bytes(12):break
            time.sleep(.02)
        else:raise RuntimeError('Neutral bootstrap readback failed')
        result['neutral_bootstrap_proven']=True
        start=time.perf_counter();events=timeline();i=0;last_tick=start
        while time.perf_counter()-start<=events[-1][0]:
            now=time.perf_counter();elapsed=now-start
            if now-last_tick>.25:raise RuntimeError('Controller scheduler delayed over250ms')
            last_tick=now;guard.check()
            while i+1<len(events) and elapsed>=events[i+1][0]:i+=1
            s=events[i][1];pad.reset()
            for mask,name in [(A,'XUSB_GAMEPAD_A'),(B,'XUSB_GAMEPAD_B'),(LB,'XUSB_GAMEPAD_LEFT_SHOULDER'),(R3,'XUSB_GAMEPAD_RIGHT_THUMB')]:
                if s['buttons']&mask:pad.press_button(getattr(module.XUSB_BUTTON,name))
            pad.left_joystick(s['lx'],s['ly']);pad.right_joystick(s['rx'],s['ry']);pad.left_trigger(s['lt']);pad.right_trigger(s['rt']);pad.update()
            value=PadState();error=api.XInputGetState(slot,ctypes.byref(value))
            result['reports'].append(dict(at_seconds=elapsed,requested=s.copy(),xinput_error=error,readback={name:getattr(value.report,name) for name,_ in PadReport._fields_}))
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
                    value=PadState();error=api.XInputGetState(slot,ctypes.byref(value))
                    if error==1167:break
                    time.sleep(.02)
                result['post_disconnect_xinput_error']=error;result['disconnect_readback_proven']=error==1167
        if guard is not None:guard.close()
        result['ended_utc']=datetime.now(timezone.utc).isoformat();result['driver_report_count']=len(result['reports'])
        with output.open('x',encoding='utf-8') as f:json.dump(result,f,indent=2)
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--execute',action='store_true');p.add_argument('--pid',type=int)
    p.add_argument('--output',type=Path);p.add_argument('--scenario',choices=['smoke'],default='smoke')
    result=run(p.parse_args());print(json.dumps({k:v for k,v in result.items() if k!='reports'},indent=2))
    if result.get('failure'):sys.exit(1)
