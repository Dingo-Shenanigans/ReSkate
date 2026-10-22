"""Bounded, explicitly opted-in virtual Xbox controller input for the isolated BMX lab.

Default is a fast dry-run: no vgamepad import, driver connection, game inspection,
window activation, keyboard injection, capture or quality verdict. Python3.11+.
"""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
import hashlib
import importlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import sys
import time
import uuid

WORKSPACE = Path(__file__).resolve().parents[2]
EXACT_WORKSPACE = r"C:\Users\Jeffr\Desktop\reskate"
GAME_SHA256 = "FBCE74D5E28EF525DBBA2CB4ADBEBC13405BDBD88F31BC940BCA45E4AE88B8F9"
PERIOD_MS, POLL_MS, MAX_LATE_MS, MAX_DURATION_MS = 20, 10, 120, 6000
BOOTSTRAP_LIMIT_MS = 1000
START_GATE_LIMIT_MS = 60000
A, B, Y, LB, RB, R3 = 0x1000, 0x2000, 0x8000, 0x0100, 0x0200, 0x0080
ALLOWED_BUTTONS = A | B | Y | LB | RB | R3
DEPENDENCY_ROOT = WORKSPACE / "build" / "bmx-gamepad-runtime" / "Lib" / "site-packages" / "vgamepad"
DEPENDENCY_PYTHON = WORKSPACE / "build" / "bmx-gamepad-runtime" / "Scripts" / "python.exe"
DEPENDENCY_RECEIPT = WORKSPACE / "build" / "bmx" / "gamepad-setup" / "dependency-verification.json"
DEPENDENCY_ARCHIVE_SHA256 = "57F6BD01AEC0C172947517FB782D150EF9B285F7F4D524C317374FA5C24A89DE"
# Six exact files independently hashed from the reviewed PyPI0.1.0 sdist.
# setup.py and the bundled driver MSI are deliberately absent.
DEPENDENCY_FILES = {
    "__init__.py": ("77EB34B1D35C255DFF78D1206E1C61400B12048AABDDD9DA5FA27FB350848D11", 334),
    "win/__init__.py": ("E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855", 0),
    "win/vigem_client.py": ("9598A035E44740715B11B1BA3FABA29B7FD1753BCA73DFA1AEC0EA514D9B02DB", 10619),
    "win/vigem_commons.py": ("BE76CF56FC5D1E07E070C76FE0D0BD846603C89387EC8FF3879713977A68829C", 6470),
    "win/virtual_gamepad.py": ("AA2211E4A17DCC34244DC5767D79EC61A285E8D47CF744BED1015E9F46CEEE53", 13590),
    "win/vigem/client/x64/ViGEmClient.dll": ("2BF0CB1D809039573C922737D298A1653D4DBC61408060FF45A9BCFDE82E97D2", 130048),
}


class Refused(RuntimeError):
    pass


@dataclass(frozen=True)
class State:
    buttons: int = 0
    left_x: float = 0.0
    left_y: float = 0.0
    right_x: float = 0.0
    right_y: float = 0.0
    left_trigger: float = 0.0
    right_trigger: float = 0.0


NEUTRAL = State()


@dataclass(frozen=True)
class Event:
    at_ms: int
    state: State


def scenarios() -> dict[str, tuple[Event, ...]]:
    # Timelines request inputs only. Starting equipment, grounded/airborne state,
    # animation, controller slot selection and consumption are separate evidence.
    return {
        "native-y": (Event(0, NEUTRAL), Event(240, State(Y)), Event(440, NEUTRAL), Event(3600, NEUTRAL)),
        # Requires a separately verified mounted rider on flat supported ground.
        # Release first, pedal, coast, request one300ms direct pump, then settle.
        "ground-pump": (Event(0, NEUTRAL), Event(240, State(A)), Event(1000, NEUTRAL),
                        Event(1200, State(left_y=-0.7)), Event(1500, NEUTRAL), Event(3200, NEUTRAL)),
        # Same direct-pump request, then brake and one normal equipment chord.
        # A successful native park may seal a trace; verify that separately.
        "ground-pump-switch": (Event(0, NEUTRAL), Event(240, State(A)), Event(1000, NEUTRAL),
                               Event(1200, State(left_y=-0.7)), Event(1500, State(B)),
                               Event(2300, State(LB)), Event(2380, State(LB | Y)),
                               Event(2580, State(LB)), Event(2660, NEUTRAL), Event(4000, NEUTRAL)),
        "equipment-switch": (Event(0, NEUTRAL), Event(240, State(LB)), Event(320, State(LB | Y)),
                             Event(520, State(LB)), Event(600, NEUTRAL), Event(4400, NEUTRAL)),
        "bunnyhop-tailwhip": (Event(0, NEUTRAL), Event(240, State(A)), Event(1760, NEUTRAL),
                             Event(1880, State(right_y=-1)), Event(2220, State(right_y=1)),
                             Event(2300, NEUTRAL), Event(2420, State(right_x=1, left_trigger=1, right_trigger=1)),
                             Event(2600, NEUTRAL), Event(5400, NEUTRAL)),
        "quickhop-barspin": (Event(0, NEUTRAL), Event(240, State(A)), Event(1760, NEUTRAL),
                             Event(1880, State(R3)), Event(2000, NEUTRAL),
                             Event(2180, State(LB, right_y=-1)), Event(2360, NEUTRAL), Event(5200, NEUTRAL)),
    }


def validate_timeline(events: tuple[Event, ...]) -> None:
    if not 2 <= len(events) <= 32 or events[0] != Event(0, NEUTRAL) or events[-1].state != NEUTRAL:
        raise Refused("Timeline requires 2..32 events with initial/final neutral")
    previous = -1
    for event in events:
        if type(event.at_ms) is not int or event.at_ms <= previous or event.at_ms % PERIOD_MS or not 0 <= event.at_ms <= MAX_DURATION_MS:
            raise Refused("Timeline must increase on20ms boundaries within6s")
        previous = event.at_ms
        state = event.state
        if not isinstance(state, State) or type(state.buttons) is not int or state.buttons < 0 or state.buttons & ~ALLOWED_BUTTONS:
            raise Refused("Unsupported button, D-pad or menu input")
        for name in ("left_x", "left_y", "right_x", "right_y", "left_trigger", "right_trigger"):
            value = getattr(state, name)
            low = 0 if "trigger" in name else -1
            if type(value) not in (int, float) or not math.isfinite(value) or not low <= value <= 1:
                raise Refused("Invalid axis/trigger")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def inside_build(path: Path, *, existing: bool = False) -> Path:
    root = WORKSPACE / "build" / "bmx"
    absolute = path.absolute()
    resolved = path.resolve(strict=existing)
    if not resolved.is_relative_to(root.resolve()) or resolved == root.resolve():
        raise Refused("Owner/ledger must be inside this workspace's build/bmx")
    cursor = absolute
    while cursor != WORKSPACE:
        if cursor.is_symlink() or (hasattr(cursor, "is_junction") and cursor.is_junction()):
            raise Refused("Reparse paths are unsupported")
        if cursor == cursor.parent:
            raise Refused("Path escaped workspace")
        cursor = cursor.parent
    return resolved


@dataclass(frozen=True)
class Owner:
    process_id: int
    process_creation_filetime: int
    game_exe: str
    runtime_sha256: str


class StartGate:
    """Fresh, nonce-bound file coordination; never changes focus or sends input.

    The caller closes the game console and atomically renames a complete go.json
    into this newly created directory. A valid trigger is an operator assertion,
    not proof that the console closed or a native observer armed.
    """
    def __init__(self, path: Path, nonce: str, owner: Owner, scenario: str, source_sha256: str):
        if not isinstance(nonce, str) or not re.fullmatch(r"[0-9a-fA-F]{32}", nonce):
            raise Refused("Start nonce must contain exactly32 hexadecimal characters")
        self.path = inside_build(path)
        if self.path.exists():
            raise Refused("Start-gate directory must be fresh and nonexistent")
        self.path.mkdir(parents=True, exist_ok=False)
        inside_build(self.path, existing=True)
        info = self.path.stat()
        self.directory_identity = (info.st_dev, info.st_ino)
        self.nonce, self.owner, self.scenario, self.source_sha256 = nonce, owner, scenario, source_sha256
        self.ready, self.go = self.path / "ready.json", self.path / "go.json"
        self.check_directory()

    def check_directory(self):
        inside_build(self.path, existing=True)
        info = self.path.stat()
        if not self.path.is_dir() or (info.st_dev, info.st_ino) != self.directory_identity:
            raise Refused("Start-gate directory identity changed")
        for path in (self.ready, self.go):
            inside_build(path)

    def trigger_fields(self):
        return dict(schema=1, event="bmx_virtual_gamepad_go", process_id=self.owner.process_id,
                    process_creation_filetime=str(self.owner.process_creation_filetime), nonce=self.nonce)

    def read_trigger(self):
        self.check_directory()
        if not self.go.exists():
            return False
        inside_build(self.go, existing=True)
        before = self.go.stat()
        if not self.go.is_file() or not 1 <= before.st_size <= 8192:
            raise Refused("Start trigger must be a regular JSON file within8KiB")
        raw = self.go.read_bytes()
        after = self.go.stat()
        token = lambda item: (item.st_dev, item.st_ino, item.st_size, item.st_mtime_ns, item.st_ctime_ns)
        self.check_directory()
        if token(before) != token(after) or len(raw) != before.st_size:
            raise Refused("Start trigger changed during read; publish it by atomic rename")
        try:
            value = json.loads(raw.decode("utf-8-sig"))
        except (ValueError, UnicodeError) as error:
            raise Refused("Malformed start trigger JSON") from error
        expected = self.trigger_fields()
        if (not isinstance(value, dict) or set(value) != set(expected) or
                type(value.get("schema")) is not int or type(value.get("process_id")) is not int or value != expected):
            raise Refused("Start trigger schema, event, owner or nonce mismatch")
        return True

    def wait(self, guard, ledger, clock):
        started, previous = clock.now(), clock.now()
        if not math.isfinite(started) or not math.isfinite(previous) or previous < started:
            raise Refused("Start-gate clock is invalid")
        guard.check()
        self.check_directory()
        if self.ready.exists() or self.go.exists():
            raise Refused("Preexisting ready/go file refused before start-gate publication")
        payload = dict(schema=1, event="bmx_virtual_gamepad_ready", process_id=self.owner.process_id,
                       process_creation_filetime=str(self.owner.process_creation_filetime), game_exe=self.owner.game_exe,
                       runtime_sha256=self.owner.runtime_sha256, nonce=self.nonce, scenario=self.scenario,
                       utc=datetime.now(timezone.utc).isoformat(), source_sha256=self.source_sha256,
                       neutral_bootstrap_completed=True, wait_limit_ms=START_GATE_LIMIT_MS)
        with self.ready.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(json.dumps(payload, separators=(",", ":")) + "\n")
            stream.flush()
        ledger.write("controller_start_gate_ready", ready_path=str(self.ready), ready=payload)
        ledger.write("controller_start_gate_wait", wait_limit_ms=START_GATE_LIMIT_MS,
                     boundary="Neutral device retained; no scheduled scenario reports before matching go.json")
        while True:
            guard.check()
            now = clock.now()
            if not math.isfinite(now) or now < previous or (now - started) * 1000 >= START_GATE_LIMIT_MS:
                raise Refused("Start-gate deadline/clock failed")
            previous = now
            accepted = self.read_trigger()
            guard.check()
            now = clock.now()
            if not math.isfinite(now) or now < previous or (now - started) * 1000 >= START_GATE_LIMIT_MS:
                raise Refused("Start trigger read exceeded deadline/clock")
            previous = now
            if accepted:
                ledger.write("controller_start_gate_go", go_path=str(self.go), nonce=self.nonce,
                             waited_ms=round((now - started) * 1000, 4), input_consumed=None)
                return
            clock.sleep(POLL_MS / 1000)


def load_owner(path: Path) -> Owner:
    path = inside_build(path, existing=True)
    if path.stat().st_size > 8192:
        raise Refused("Owner receipt exceeds8KiB")
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    pid, created = value.get("process_id"), value.get("process_creation_filetime")
    expected_exe = str(WORKSPACE / "build" / "bmx-game" / "Skate.exe")
    if value.get("schema") != 1 or type(value.get("schema")) is not int or value.get("event") != "bmx_virtual_gamepad_owner":
        raise Refused("A dedicated explicit virtual-gamepad owner receipt is required")
    if type(pid) is not int or not 1 <= pid <= 0xFFFFFFFF:
        raise Refused("Invalid owned PID")
    if not isinstance(created, str) or not re.fullmatch(r"[1-9][0-9]{0,19}", created) or int(created) > 0xFFFFFFFFFFFFFFFF:
        raise Refused("Exact canonical Windows creation FILETIME is required")
    if not isinstance(value.get("game_exe"), str) or os.path.normcase(value["game_exe"]) != os.path.normcase(expected_exe):
        raise Refused("Owner must name the exact isolated game path")
    digest = value.get("runtime_sha256")
    if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-fA-F]{64}", digest):
        raise Refused("Expected runtime SHA256 is required")
    return Owner(pid, int(created), expected_exe, digest.upper())


class WindowsGuard:
    """Read-only process metadata, disk identity, foreground and Escape checks.

    No ReadProcessMemory, writes, focus changes or OS keyboard input. Module
    enumeration confirms loaded path; disk SHA is not an in-memory image hash.
    """
    def __init__(self, owner: Owner):
        if os.name != "nt" or os.path.normcase(str(WORKSPACE)) != os.path.normcase(EXACT_WORKSPACE):
            raise Refused("Execution requires the exact authorized Windows workspace")
        self.owner, self.handle = owner, None
        self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        self.user = ctypes.WinDLL("user32", use_last_error=True)
        self.kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.kernel.OpenProcess.restype = wintypes.HANDLE
        self.kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel.CloseHandle.restype = wintypes.BOOL
        self.kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        self.kernel.GetProcessTimes.restype = wintypes.BOOL
        self.kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
        self.kernel.GetExitCodeProcess.restype = wintypes.BOOL
        self.kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
        self.kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
        self.user.GetForegroundWindow.restype = wintypes.HWND
        self.user.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        self.user.GetWindowThreadProcessId.restype = wintypes.DWORD
        self.user.GetAsyncKeyState.argtypes = [ctypes.c_int]
        self.user.GetAsyncKeyState.restype = ctypes.c_short
        self.runtime = WORKSPACE / "build" / "bmx-game" / "ReSkate.dll"
        game = Path(owner.game_exe)
        for path in (game, self.runtime):
            cursor = path
            while cursor != WORKSPACE:
                if cursor.is_symlink() or (hasattr(cursor, "is_junction") and cursor.is_junction()):
                    raise Refused("Isolated executable/runtime reparse paths refused")
                if cursor == cursor.parent:
                    raise Refused("Isolated file escaped the workspace")
                cursor = cursor.parent
        if sha256(game) != GAME_SHA256 or sha256(self.runtime) != owner.runtime_sha256:
            raise Refused("Isolated game/runtime disk identity mismatch")
        self.file_tokens = self._file_tokens()
        self.last_modules = 0.0
        try:
            self.handle = self.kernel.OpenProcess(0x1000, False, owner.process_id)
            if not self.handle:
                raise Refused("Owned process metadata is unavailable")
            self.check()
        except BaseException:
            self.close()
            raise

    def _file_tokens(self):
        # One coherent snapshot per file; preserve every existing token/check.
        stats = (p.stat() for p in (Path(self.owner.game_exe), self.runtime))
        return tuple((s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns) for s in stats)

    def _runtime_loaded(self):
        class Module(ctypes.Structure):
            _fields_ = [("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                        ("GlblcntUsage", wintypes.DWORD), ("ProccntUsage", wintypes.DWORD), ("modBaseAddr", ctypes.POINTER(ctypes.c_ubyte)),
                        ("modBaseSize", wintypes.DWORD), ("hModule", wintypes.HMODULE), ("szModule", wintypes.WCHAR * 256),
                        ("szExePath", wintypes.WCHAR * 260)]
        self.kernel.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
        self.kernel.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        self.kernel.Module32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Module)]
        self.kernel.Module32FirstW.restype = wintypes.BOOL
        self.kernel.Module32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(Module)]
        self.kernel.Module32NextW.restype = wintypes.BOOL
        snapshot = self.kernel.CreateToolhelp32Snapshot(0x08 | 0x10, self.owner.process_id)
        if snapshot == ctypes.c_void_p(-1).value:
            raise Refused("Loaded runtime module list unavailable")
        try:
            module = Module()
            module.dwSize = ctypes.sizeof(Module)
            present = self.kernel.Module32FirstW(snapshot, ctypes.byref(module))
            for _ in range(512):
                if not present:
                    break
                if os.path.normcase(module.szExePath) == os.path.normcase(str(self.runtime)) and module.modBaseSize > 0:
                    return
                present = self.kernel.Module32NextW(snapshot, ctypes.byref(module))
            raise Refused("Exact isolated ReSkate.dll is not enumerated as loaded")
        finally:
            self.kernel.CloseHandle(snapshot)

    def check(self):
        if self.user.GetAsyncKeyState(0x1B) & 0x8000:
            raise Refused("Escape abort")
        foreground_pid = wintypes.DWORD()
        window = self.user.GetForegroundWindow()
        if not window or not self.user.GetWindowThreadProcessId(window, ctypes.byref(foreground_pid)) or foreground_pid.value != self.owner.process_id:
            raise Refused("Owned game lost foreground")
        exit_code = wintypes.DWORD()
        if not self.handle or not self.kernel.GetExitCodeProcess(self.handle, ctypes.byref(exit_code)) or exit_code.value != 259:
            raise Refused("Owned game ended or process identity unavailable")
        created, ended, kernel, user = (wintypes.FILETIME() for _ in range(4))
        if not self.kernel.GetProcessTimes(self.handle, ctypes.byref(created), ctypes.byref(ended), ctypes.byref(kernel), ctypes.byref(user)):
            raise Refused("Owned creation time unavailable")
        if (created.dwHighDateTime << 32 | created.dwLowDateTime) != self.owner.process_creation_filetime:
            raise Refused("Owned creation FILETIME changed")
        name, length = ctypes.create_unicode_buffer(32768), wintypes.DWORD(32768)
        if not self.kernel.QueryFullProcessImageNameW(self.handle, 0, name, ctypes.byref(length)) or os.path.normcase(name.value) != os.path.normcase(self.owner.game_exe):
            raise Refused("Owned executable path changed")
        if self._file_tokens() != self.file_tokens:
            raise Refused("Isolated game/runtime files changed")
        now = time.monotonic()
        if not self.last_modules or now - self.last_modules >= 0.20:
            self._runtime_loaded()
            self.last_modules = now

    def close(self):
        if self.handle:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


class XInputReport(ctypes.Structure):
    _fields_ = [("buttons", ctypes.c_ushort), ("lt", ctypes.c_ubyte), ("rt", ctypes.c_ubyte),
                ("lx", ctypes.c_short), ("ly", ctypes.c_short), ("rx", ctypes.c_short), ("ry", ctypes.c_short)]


class XInputState(ctypes.Structure):
    _fields_ = [("packet", wintypes.DWORD), ("report", XInputReport)]


class WindowsControllerProbe:
    def __init__(self):
        self.client = importlib.import_module("vgamepad.win.vigem_client")
        self.api = ctypes.WinDLL(r"C:\Windows\System32\xinput1_4.dll", use_last_error=True)
        self.api.XInputGetState.argtypes = [wintypes.DWORD, ctypes.POINTER(XInputState)]
        self.api.XInputGetState.restype = wintypes.DWORD
        if ctypes.sizeof(XInputReport) != 12 or ctypes.sizeof(XInputState) != 16:
            raise Refused("Unexpected Windows XInput structure ABI")

    def read(self, pad):
        slot = wintypes.DWORD(0xFFFFFFFF)
        status = int(self.client.vigem_target_x360_get_user_index(pad._busp, pad._devicep, ctypes.byref(slot)))
        if status != 0x20000000 or slot.value > 3:
            return dict(available=False, issue="driver_user_index_unavailable", driver_status=status, slot=None, neutral=False)
        state = XInputState()
        result = int(self.api.XInputGetState(slot.value, ctypes.byref(state)))
        if result:
            return dict(available=False, issue="xinput_slot_unavailable", xinput_status=result, slot=slot.value, neutral=False)
        raw = ctypes.string_at(ctypes.byref(state.report), ctypes.sizeof(state.report))
        return dict(available=True, slot=slot.value, packet=state.packet, raw_report_hex=raw.hex(),
                    report={name: getattr(state.report, name) for name, _ in XInputReport._fields_}, neutral=raw == bytes(12))


class VGamepadBackend:
    def __init__(self, module, *, probe=None):
        # Vetted vgamepad0.1.0 exposes removal through __del__, not public close.
        # This owner makes that removal/free idempotent; no raw ViGEm call here.
        class ManagedPad(module.VX360Gamepad):
            def __init__(self):
                self.harness_closed = False
                super().__init__()

            def close(self):
                if not self.harness_closed:
                    self.harness_closed = True
                    super().__del__()

            def __del__(self):
                if getattr(self, "_devicep", None) and getattr(self, "_busp", None):
                    self.close()

        self.probe = probe if probe is not None else WindowsControllerProbe()
        self.buttons = [(mask, getattr(module.XUSB_BUTTON, name)) for mask, name in (
            (A, "XUSB_GAMEPAD_A"), (B, "XUSB_GAMEPAD_B"), (Y, "XUSB_GAMEPAD_Y"),
            (LB, "XUSB_GAMEPAD_LEFT_SHOULDER"), (RB, "XUSB_GAMEPAD_RIGHT_SHOULDER"), (R3, "XUSB_GAMEPAD_RIGHT_THUMB"))]
        self.pad = ManagedPad()

    def prime_lsb(self):
        self.pad.reset()
        self.pad.left_joystick(x_value=1, y_value=0)
        self.pad.update()

    def raw_report_hex(self):
        return ctypes.string_at(ctypes.byref(self.pad.report), ctypes.sizeof(self.pad.report)).hex()

    def readback(self):
        return self.probe.read(self.pad)

    def send(self, state: State):
        self.pad.reset()
        for mask, button in self.buttons:
            if state.buttons & mask:
                self.pad.press_button(button=button)
        self.pad.left_joystick_float(x_value_float=state.left_x, y_value_float=state.left_y)
        self.pad.right_joystick_float(x_value_float=state.right_x, y_value_float=state.right_y)
        self.pad.left_trigger_float(value_float=state.left_trigger)
        self.pad.right_trigger_float(value_float=state.right_trigger)
        self.pad.update()

    def close(self):
        self.pad.close()


def validate_dependency(path: Path):
    path = inside_build(path, existing=True)
    if path.stat().st_size > 32768:
        raise Refused("Dependency receipt exceeds32KiB")
    receipt = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(receipt, dict) or not all(isinstance(receipt.get(name), str) for name in ("source_sha256", "package_root", "python_exe")):
        raise Refused("Dependency receipt must contain typed provenance fields")
    if (type(receipt.get("schema")) is not int or receipt["schema"] != 1 or
            receipt.get("event") != "bmx_virtual_gamepad_dependency" or receipt.get("version") != "0.1.0" or
            receipt.get("source_sha256", "").upper() != DEPENDENCY_ARCHIVE_SHA256 or
            receipt.get("setup_py_executed") is not False or receipt.get("bundled_driver_installer_executed") is not False):
        raise Refused("Reviewed manual dependency provenance is required")
    if (os.path.normcase(receipt.get("package_root", "")) != os.path.normcase(str(DEPENDENCY_ROOT)) or
            os.path.normcase(receipt.get("python_exe", "")) != os.path.normcase(str(DEPENDENCY_PYTHON)) or
            os.path.normcase(sys.executable) != os.path.normcase(str(DEPENDENCY_PYTHON))):
        raise Refused("Exact private dependency package/interpreter required")
    rows = receipt.get("files")
    if not isinstance(rows, list) or len(rows) != len(DEPENDENCY_FILES):
        raise Refused("Expected the exact six reviewed backend files")
    declared = {}
    for row in rows:
        if not isinstance(row, dict) or not isinstance(row.get("relative_path"), str):
            raise Refused("Dependency file entries must have relative paths")
        name = row.get("relative_path")
        if name in declared or name not in DEPENDENCY_FILES:
            raise Refused("Unexpected or duplicate dependency file")
        declared[name] = row
    cursor = DEPENDENCY_ROOT
    while cursor != WORKSPACE:
        if cursor.is_symlink() or (hasattr(cursor, "is_junction") and cursor.is_junction()):
            raise Refused("Dependency ancestor reparse path refused")
        if cursor == cursor.parent:
            raise Refused("Dependency package escaped workspace")
        cursor = cursor.parent
    actual = set()
    for item in DEPENDENCY_ROOT.rglob("*"):
        if item.is_symlink() or (hasattr(item, "is_junction") and item.is_junction()):
            raise Refused("Dependency reparse paths refused")
        if item.is_file():
            actual.add(item.relative_to(DEPENDENCY_ROOT).as_posix())
    if actual != set(DEPENDENCY_FILES):
        raise Refused("Unexpected package file or bytecode cache; preserve exact source-only package")
    for name, (digest, size) in DEPENDENCY_FILES.items():
        row, file = declared[name], DEPENDENCY_ROOT / name
        if (type(row.get("bytes")) is not int or row["bytes"] != size or not isinstance(row.get("sha256"), str) or row["sha256"].upper() != digest or
                file.stat().st_size != size or sha256(file) != digest):
            raise Refused("Reviewed backend file hash/size mismatch: " + name)
    return receipt


def make_vgamepad(receipt_path: Path):
    # No setup.py, pip, MSI or metadata import. The manual-copy package has no
    # dist-info; its six source/client files are pinned and checked pre-import.
    receipt = validate_dependency(receipt_path)
    receipt_hash = sha256(receipt_path)
    if any(name == "vgamepad" or name.startswith("vgamepad.") for name in sys.modules):
        raise Refused("Use a fresh harness process; vgamepad was already imported")
    spec = importlib.util.find_spec("vgamepad")
    if spec is None or spec.origin is None or Path(spec.origin).resolve() != (DEPENDENCY_ROOT / "__init__.py").resolve():
        raise Refused("Python would import an unreviewed vgamepad package")
    # Do not generate package bytecode: every future execution rechecks six files.
    sys.dont_write_bytecode = True
    backend = VGamepadBackend(importlib.import_module("vgamepad"))
    backend.dependency = dict(version=receipt["version"], source_sha256=DEPENDENCY_ARCHIVE_SHA256,
                              receipt_sha256=receipt_hash, package_root=str(DEPENDENCY_ROOT),
                              client_sha256=DEPENDENCY_FILES["win/vigem/client/x64/ViGEmClient.dll"][0])
    return backend


class DryBackend:
    def send(self, state):
        pass

    def close(self):
        pass


class DryGuard:
    def check(self):
        pass

    def close(self):
        pass


class Clock:
    # Python3.12 Windows monotonic uses15.625ms GetTickCount64. QPC is monotonic
    # and measures the same real seconds without quantizing20ms deadlines.
    now = staticmethod(time.perf_counter)
    sleep = staticmethod(time.sleep)


class PreviewClock:
    def __init__(self):
        self.value = 0.0

    def now(self):
        return self.value

    def sleep(self, seconds):
        self.value += seconds


class Ledger:
    def __init__(self, path: Path):
        self.stream = path.open("x", encoding="utf-8", newline="\n")
        self.lines = 0

    def write(self, event, **fields):
        self.lines += 1
        if self.lines > 512:
            raise Refused("Bounded ledger row limit exceeded")
        row = dict(event=event, utc=datetime.now(timezone.utc).isoformat(), host_monotonic_ns=time.monotonic_ns(), **fields)
        self.stream.write(json.dumps(row, allow_nan=False, separators=(",", ":")) + "\n")
        self.stream.flush()

    def close(self):
        self.stream.close()


def bootstrap(backend, guard, ledger, clock):
    """One explicit raw-stick LSB, then bounded exact-neutral verification.

    ViGEm's USB boot blob contains fixed nonzero axes, while its duplicate cache
    starts zero. Sending only zero can be suppressed without clearing the blob.
    This phase is declared virtual input, separate from the test timeline.
    """
    started = clock.now()
    if not math.isfinite(started):
        raise Refused("Bootstrap clock is not finite")
    previous, slot, zero_count = started, None, 0
    ledger.write("controller_bootstrap_attempt", operation="raw_left_x_one_lsb", desired_raw_report_hex="000000000100000000000000",
                 driver_submitted=None, input_consumed=None)
    guard.check()
    backend.prime_lsb()
    ledger.write("controller_bootstrap_report", operation="raw_left_x_one_lsb", left_x_raw=1, buttons=0,
                 raw_report_hex=backend.raw_report_hex(), driver_submitted=True, input_consumed=None)
    for _ in range(BOOTSTRAP_LIMIT_MS // PERIOD_MS + 1):
        now = clock.now()
        if not math.isfinite(now) or now < previous or (now - started) * 1000 >= BOOTSTRAP_LIMIT_MS:
            raise Refused("Bootstrap neutral verification deadline/clock failed")
        previous = now
        ledger.write("controller_bootstrap_attempt", operation="neutral", desired_raw_report_hex="00" * 12,
                     driver_submitted=None, input_consumed=None)
        guard.check()
        backend.send(NEUTRAL)
        ledger.write("controller_bootstrap_report", operation="neutral", raw_report_hex=backend.raw_report_hex(),
                     driver_submitted=True, input_consumed=None)
        guard.check()
        observed = backend.readback()
        ledger.write("controller_bootstrap_readback", **observed, input_consumed=None)
        guard.check()
        now = clock.now()
        if not math.isfinite(now) or now < previous or (now - started) * 1000 >= BOOTSTRAP_LIMIT_MS:
            raise Refused("Bootstrap readback exceeded deadline/clock")
        previous = now
        if observed.get("available"):
            current_slot = observed.get("slot")
            if type(current_slot) is not int or not 0 <= current_slot <= 3 or (slot is not None and current_slot != slot):
                raise Refused("Driver-assigned controller slot changed")
            slot = current_slot
        exact_zero = observed.get("available") is True and observed.get("neutral") is True and observed.get("raw_report_hex") == "00" * 12
        zero_count = zero_count + 1 if exact_zero else 0
        if zero_count == 2:
            ledger.write("controller_bootstrap_complete", slot=slot, exact_zero_samples=2,
                         elapsed_ms=round((clock.now() - started) * 1000, 4), input_consumed=None)
            return
        for _ in range(PERIOD_MS // POLL_MS):
            guard.check()
            clock.sleep(POLL_MS / 1000)
    raise Refused("Bootstrap did not establish exact neutral")


def run(events, guard, backend_factory, ledger, *, execute=False, clock=None, start_gate=None):
    validate_timeline(events)
    if start_gate is not None and not execute:
        raise Refused("Start gate is available only with explicit execution")
    clock = clock or Clock()
    backend, issue, reports = None, None, 0
    factory_started = False
    released = disconnected = False
    try:
        guard.check()
        factory_started = True
        backend = backend_factory()
        ledger.write("controller_backend_ready", driver_execution_requested=execute,
                     dependency=getattr(backend, "dependency", None))
        if execute:
            bootstrap(backend, guard, ledger, clock)
        if start_gate is not None:
            start_gate.wait(guard, ledger, clock)
        start, current, index = clock.now(), events[0].state, 0
        if not math.isfinite(start):
            raise Refused("Scheduler clock is not finite")
        last_clock = start

        def now_checked():
            nonlocal last_clock
            now = clock.now()
            if not math.isfinite(now) or now < last_clock:
                raise Refused("Scheduler clock reversed or became nonfinite")
            last_clock = now
            return now

        for at_ms in range(0, events[-1].at_ms + 1, PERIOD_MS):
            deadline = start + at_ms / 1000
            now = now_checked()
            while now < deadline:
                guard.check()
                # The guard can consume the remaining interval. Recheck after
                # it before deciding whether any sleep is still required.
                now = now_checked()
                if now < deadline:
                    clock.sleep(min(POLL_MS / 1000, deadline - now))
                    now = now_checked()
            guard.check()
            elapsed_ms = (now_checked() - start) * 1000
            if elapsed_ms < 0 or elapsed_ms - at_ms > MAX_LATE_MS:
                raise Refused("Report scheduler clock reversed or exceeded120ms lateness")
            while index + 1 < len(events) and events[index + 1].at_ms <= at_ms:
                index += 1
                current = events[index].state
            backend.send(current)
            reports += 1
            ledger.write("controller_report", planned_ms=at_ms, elapsed_ms=round(elapsed_ms, 4),
                         state=asdict(current), raw_report_hex=backend.raw_report_hex() if execute else None,
                         driver_submitted=execute, input_consumed=None)
    except BaseException as error:
        issue = f"{type(error).__name__}: {error}"
    finally:
        if backend is not None:
            try:
                backend.send(NEUTRAL)
                released = True
            except BaseException as error:
                issue = issue or f"Neutral release failed: {error}"
            try:
                ledger.write("controller_neutral_release", state=asdict(NEUTRAL), succeeded=released,
                             raw_report_hex=backend.raw_report_hex() if execute and released else None,
                             driver_submitted=(True if released else None) if execute else False, input_consumed=None)
            except BaseException as error:
                issue = issue or f"Neutral ledger write failed: {error}"
            try:
                backend.close()
                disconnected = True
            except BaseException as error:
                issue = issue or f"Controller disconnect failed: {error}"
        try:
            guard.close()
        except BaseException as error:
            issue = issue or f"Process guard close failed: {error}"
        result = dict(completed=issue is None, issue=issue, report_count=reports,
                      neutral_release_succeeded=released, disconnect_succeeded=disconnected,
                      driver_execution_requested=execute, driver_report_count=reports if execute else 0,
                      driver_submitted=(True if backend is not None else None) if execute and factory_started else False,
                      backend_factory_attempted=factory_started, input_consumed=None, native_quality=None)
        try:
            ledger.write("controller_session_end", **result)
        except BaseException as error:
            result.update(completed=False, issue=f"Ledger finalization failed: {error}; prior issue={issue}")
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=scenarios(), default="native-y")
    parser.add_argument("--execute", action="store_true", help="Explicitly submit virtual controller reports; default is dry-run")
    parser.add_argument("--owner", type=Path, help="Dedicated reviewed owned-process JSON receipt; mandatory for execution")
    parser.add_argument("--dependency-receipt", type=Path, default=DEPENDENCY_RECEIPT, help="Reviewed six-file manual dependency receipt; checked before live import")
    parser.add_argument("--ledger", type=Path, help="New JSONL path inside build/bmx; never overwrites")
    parser.add_argument("--start-gate", type=Path, help="Fresh nonexistent directory inside build/bmx for optional ready/go coordination")
    parser.add_argument("--start-nonce", help="Exactly32 hexadecimal characters binding the optional start gate")
    args = parser.parse_args(argv)
    if bool(args.start_gate) != bool(args.start_nonce) or (args.start_gate is not None and not args.execute):
        parser.error("--start-gate and --start-nonce require each other and --execute")
    owner = load_owner(args.owner) if args.owner else None
    if args.execute and owner is None:
        parser.error("--execute requires an exact owned-process --owner receipt")
    ledger_path = inside_build(args.ledger or WORKSPACE / "build" / "bmx" / "virtual-gamepad" /
                               (datetime.now(timezone.utc).strftime("%Y%m%d-%H%M%S-") + uuid.uuid4().hex + ".jsonl"))
    ledger_path.parent.mkdir(parents=True, exist_ok=True)
    ledger = Ledger(ledger_path)
    guard = None
    try:
        ledger.write("controller_session_start", schema=1, scenario=args.scenario, execute=args.execute,
                     timing_simulated=not args.execute, owner=asdict(owner) if owner else None,
                     timeline=[asdict(e) for e in scenarios()[args.scenario]], source_sha256=sha256(Path(__file__)),
                     boundary="Virtual controller reports only; no proof of device selection, consumption, animation or native quality; no capture or posting")
        try:
            guard = WindowsGuard(owner) if args.execute else DryGuard()
            start_gate = StartGate(args.start_gate, args.start_nonce, owner, args.scenario, sha256(Path(__file__))) if args.start_gate else None
        except BaseException as error:
            # No backend factory has been reached on this refusal path.
            try:
                ledger.write("controller_session_refused", completed=False, issue=f"{type(error).__name__}: {error}",
                             driver_report_count=0, driver_submitted=False, input_consumed=None, native_quality=None)
            except BaseException:
                pass
            raise
        result = run(scenarios()[args.scenario], guard, (lambda: make_vgamepad(args.dependency_receipt)) if args.execute else DryBackend, ledger,
                     execute=args.execute, clock=Clock() if args.execute else PreviewClock(), start_gate=start_gate)
        print(json.dumps(dict(ledger=str(ledger_path), **result)))
        return 0 if result["completed"] else 2
    finally:
        if guard is not None:
            guard.close()
        ledger.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (Refused, OSError, ValueError, ImportError) as error:
        print(f"Virtual-gamepad refused: {error}")
        raise SystemExit(2)
