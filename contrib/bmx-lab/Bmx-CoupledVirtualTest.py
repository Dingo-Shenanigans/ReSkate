"""Coupled-trial front end; approved driver remains byte-identical.

Default is dry-run. Live operation requires genuine engine markers;
no marker is synthesized here and no recorder ACK is published here.
"""
from __future__ import annotations
import argparse
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
import time
import types

ROOT = Path(__file__).resolve().parents[2]
APPROVED_DRIVER_SHA = "2964E18026D92EBD2239E0B938D25FACE48A792F2F14C18C43EF094BE3A2AFFE"
NATIVE_H = .01666666567325592
MARKER_LIMIT = 65536
ACTIVATION_FRESH_MS = 1000
BUDGET_MS = 6000 + 120 + 500


class Refused(RuntimeError):
    pass


def load_driver():
    path = ROOT / "contrib/bmx-lab/Bmx-VirtualGamepad.py"
    raw = path.read_bytes()
    if hashlib.sha256(raw).hexdigest().upper() != APPROVED_DRIVER_SHA:
        raise Refused("Approved baseline driver source changed")
    module = types.ModuleType("reskate_approved_virtual_pad")
    module.__file__ = str(path)
    sys.modules[module.__name__] = module
    exec(compile(raw, str(path), "exec"), module.__dict__)  # No bytecode cache writes.
    return module


def scenarios(h):
    E, S, N = h.Event, h.State, h.NEUTRAL
    return {
        "coupled-neutral": (E(0, N), E(6000, N)),
        "coupled-pedal-brake": (E(0, N), E(240, S(h.A)), E(1240, N), E(2000, S(h.B)), E(2500, N), E(6000, N)),
        "coupled-steer": (E(0, N), E(240, S(h.A)), E(840, S(h.A, left_x=.25)),
                          E(1240, S(h.A, left_x=-.25)), E(1640, N), E(2400, S(h.B)), E(2800, N), E(6000, N)),
        # A request timeline only: actual airborne accepted ID34 and landing are
        # independent native evidence. Do not use until simpler trials qualify.
        "coupled-hop-barspin-left": (E(0, N), E(240, S(h.A)), E(1240, N), E(1500, S(h.R3)),
                                     E(1620, N), E(1800, S(h.LB, right_y=-1)), E(1980, N), E(6000, N)),
    }


def uint(value, maximum=(1 << 64) - 1, minimum=0):
    return type(value) is int and minimum <= value <= maximum


def real(value):
    try:
        return type(value) in (int, float) and math.isfinite(value)
    except OverflowError:
        return False


def validate_epoch(value):
    if not isinstance(value, dict) or set(value) != {"owner_world", "token", "serial", "time", "h", "bodies"}:
        raise Refused("Full native epoch is required")
    owner, bodies = value["owner_world"], value["bodies"]
    if (not isinstance(owner, list) or len(owner) != 8 or not all(uint(v, minimum=1) for v in owner) or
            not uint(value["token"], minimum=1) or not uint(value["serial"], minimum=1) or
            not real(value["time"]) or value["time"] < 0 or not real(value["h"]) or abs(value["h"] - NATIVE_H) > 1e-7 or
            not isinstance(bodies, list) or len(bodies) != 2):
        raise Refused("Native epoch clock/owner/body shape is invalid")
    for body in bodies:
        if (not isinstance(body, dict) or set(body) != {"world", "index", "generation"} or
                not uint(body["world"], minimum=1) or body["world"] != owner[5] or
                not uint(body["index"], 1048575) or not uint(body["generation"], (1 << 32) - 1)):
            raise Refused("Native body identity is invalid")
    if bodies[0]["index"] == bodies[1]["index"]:
        raise Refused("Two distinct native bodies required")
    return value


def identity(epoch):
    return epoch["owner_world"], epoch["bodies"], epoch["token"]


def no_reparse(path, root):
    path, root = path.absolute(), root.resolve(strict=True)
    if not path.resolve(strict=False).is_relative_to(root):
        raise Refused("Marker path escaped its exact root")
    cursor = path
    while cursor != root:
        if cursor.is_symlink() or (hasattr(cursor, "is_junction") and cursor.is_junction()):
            raise Refused("Marker reparse path refused")
        if cursor == cursor.parent:
            raise Refused("Marker path escaped its exact root")
        cursor = cursor.parent
    return path


def token(info):
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns


def read_json(path, root):
    no_reparse(path, root)
    before = path.stat()
    if not path.is_file() or not 0 < before.st_size <= MARKER_LIMIT:
        raise Refused("Marker must be a regular bounded JSON file")
    raw = path.read_bytes()
    after = path.stat()
    no_reparse(path, root)
    if token(before) != token(after) or len(raw) != before.st_size:
        raise Refused("Marker changed during read")
    def unique_object(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise ValueError("Duplicate marker field")
            value[key] = item
        return value
    def invalid_constant(value):
        raise ValueError("Non-finite JSON constant: " + value)
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=unique_object, parse_constant=invalid_constant)
    except (ValueError, UnicodeError) as error:
        raise Refused("Malformed marker JSON") from error
    return value, token(after), hashlib.sha256(raw).hexdigest().upper()


class MarkerMonitor:
    COMMON = {"schema", "event", "process_id", "process_creation_filetime", "capture_nonce", "operator_generation"}
    ACTIVE = COMMON | {"activation_begun_tick_ms", "activation_succeeded_tick_ms", "conservative_expiry_tick_ms", "epoch",
                       "entry_ready", "actual_spring_render_verified", "backend_active", "normal_force_adoption_enabled"}
    TERMINAL = COMMON | {"terminal_tick_ms", "issue", "epoch"}

    def __init__(self, h, owner, capture_nonce, capture_status, wall_ms):
        if not re.fullmatch(r"[0-9a-f]{32}", capture_nonce):
            raise Refused("Exact lowercase recorder nonce required")
        self.h, self.owner, self.nonce, self.wall_ms = h, owner, capture_nonce, wall_ms
        self.profile = h.WORKSPACE / "build/bmx-user-data-coupled-test"
        no_reparse(self.profile, h.WORKSPACE)
        if not self.profile.is_dir():
            raise Refused("Existing exact isolated coupled profile required")
        self.capture_status = h.inside_build(capture_status, existing=True)
        stem = f"coupled-{owner.process_id}-{owner.process_creation_filetime}-{capture_nonce}"
        self.activation = self.profile / (stem + "-activation.json")
        self.terminal = self.profile / (stem + "-terminal.json")
        if self.activation.exists() or self.terminal.exists():
            raise Refused("Preexisting activation/terminal marker refused")
        self.created_ns = time.time_ns()
        self.accepted = None
        self.active_token = None
        self.active_sha = None
        self.go_published = False
        self.previous_wall = None
        self.capture("waiting")  # Refuse before constructing a pad if already released.

    def now(self):
        value = self.wall_ms()
        if not uint(value) or (self.previous_wall is not None and value < self.previous_wall):
            raise Refused("Actual GetTickCount64 domain regressed")
        self.previous_wall = value
        return value

    def capture(self, state):
        value, _, _ = read_json(self.capture_status, self.h.WORKSPACE)
        if (not isinstance(value, dict) or type(value.get("schema_version")) is not int or value["schema_version"] != 1 or
                value.get("event") != "bmx_recording_startup_gate" or value.get("state") != state or
                value.get("nonce") != self.nonce or type(value.get("process_id")) is not int or value["process_id"] != self.owner.process_id or
                value.get("process_creation_filetime") != str(self.owner.process_creation_filetime) or
                not uint(value.get("started_tick_ms")) or not uint(value.get("elapsed_ms"))):
            raise Refused("Exact-owned recorder gate state/nonce/FILETIME required")
        return value["started_tick_ms"] + value["elapsed_ms"]

    def common(self, value, event, fields):
        if (not isinstance(value, dict) or set(value) != fields or type(value.get("schema")) is not int or value["schema"] != 1 or
                value["event"] != event or type(value["process_id"]) is not int or value["process_id"] != self.owner.process_id or
                value["process_creation_filetime"] != str(self.owner.process_creation_filetime) or value["capture_nonce"] != self.nonce or
                not uint(value["operator_generation"], minimum=1)):
            raise Refused("Marker schema/event/exact process/nonce/generation mismatch")

    def check(self):
        now = self.now()
        if self.terminal.exists():
            value, marker_token, _ = read_json(self.terminal, self.h.WORKSPACE)
            self.common(value, "coupled_trial_terminal", self.TERMINAL)
            if marker_token[3] < self.created_ns or not uint(value["terminal_tick_ms"]) or value["terminal_tick_ms"] > now:
                raise Refused("Stale/future terminal marker")
            if not isinstance(value["issue"], str) or not value["issue"].strip():
                raise Refused("Terminal issue missing")
            if value["epoch"] is not None:
                validate_epoch(value["epoch"])
            if self.accepted is not None and (value["operator_generation"] != self.accepted["operator_generation"] or
                    (value["epoch"] is not None and identity(value["epoch"]) != identity(self.accepted["epoch"]))):
                raise Refused("Terminal marker changed trial identity")
            raise Refused("Actual coupled terminal marker: " + value["issue"])
        if self.accepted is not None:
            _, current_token, current_sha = read_json(self.activation, self.h.WORKSPACE)
            if current_token != self.active_token or current_sha != self.active_sha:
                raise Refused("Accepted activation marker changed")
            if now >= self.accepted["conservative_expiry_tick_ms"]:
                raise Refused("Coupled active conservative deadline reached")

    def ready(self):
        self.check()
        if self.accepted is not None:
            return True
        if not self.activation.exists():
            return False
        value, marker_token, digest = read_json(self.activation, self.h.WORKSPACE)
        self.common(value, "coupled_trial_activated", self.ACTIVE)
        validate_epoch(value["epoch"])
        now = self.now()
        begin, success, until = (value[k] for k in ("activation_begun_tick_ms", "activation_succeeded_tick_ms", "conservative_expiry_tick_ms"))
        if (marker_token[3] < self.created_ns or not all(uint(v) for v in (begin, success, until)) or
                value["operator_generation"] > begin or not begin <= success < until or until != begin + 10000 or
                now < success or now - success > ACTIVATION_FRESH_MS or until - now < BUDGET_MS or begin < self.capture("released")):
            raise Refused("Activation is stale/future/unreleased or lacks conservative six-second budget")
        if any(value[k] is not True for k in ("entry_ready", "actual_spring_render_verified", "backend_active")) or value["normal_force_adoption_enabled"] is not False:
            raise Refused("Genuine completed entry/private activation required; normal force gate remains closed")
        self.accepted, self.active_token, self.active_sha = value, marker_token, digest
        return True


class TrialGuard:
    def __init__(self, original, monitor):
        self.original, self.monitor = original, monitor

    def check(self):
        self.original.check()  # Never weaken the exact owner/runtime/foreground/ESC guard.
        self.monitor.check()

    def close(self):
        self.original.close()


def atomic_go(path, value):
    temporary = path.with_suffix(".tmp")
    created = False
    try:
        with temporary.open("x", encoding="utf-8", newline="\n") as stream:
            created = True
            stream.write(json.dumps(value, separators=(",", ":")) + "\n")
            stream.flush()
            os.fsync(stream.fileno())
        if os.name == "nt":
            os.rename(temporary, path)  # Windows rename refuses an existing destination.
        else:
            os.link(temporary, path)  # Portable fake-only equivalent, no overwrite.
            temporary.unlink()
    finally:
        if created and temporary.exists():
            temporary.unlink()


def coordinated_gate(h, monitor, path, nonce, owner, scenario, source_sha, ledger):
    class CoupledStartGate(h.StartGate):
        def read_trigger(self):
            self.check_directory()
            if not monitor.go_published:
                if self.go.exists():
                    raise Refused("External GO without proved activation refused")
                if not monitor.ready():
                    return False
                atomic_go(self.go, self.trigger_fields())
                monitor.go_published = True
                ledger.write("coupled_activation_go", activation_sha256=monitor.active_sha,
                             activation=monitor.accepted, capture_nonce=monitor.nonce,
                             runtime_sha256_from_owned_receipt=owner.runtime_sha256, input_consumed=None)
            return super().read_trigger()
    return CoupledStartGate(path, nonce, owner, scenario, source_sha)


def windows_ticks():
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.GetTickCount64.argtypes = []
    api.GetTickCount64.restype = ctypes.c_ulonglong
    return lambda: int(api.GetTickCount64())


def main(argv=None):
    h = load_driver()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=scenarios(h), default="coupled-neutral")
    parser.add_argument("--execute", action="store_true")
    parser.add_argument("--owner", type=Path)
    parser.add_argument("--dependency-receipt", type=Path, default=h.DEPENDENCY_RECEIPT)
    parser.add_argument("--ledger", type=Path, required=True)
    parser.add_argument("--start-gate", type=Path)
    parser.add_argument("--start-nonce")
    parser.add_argument("--capture-status", type=Path)
    parser.add_argument("--capture-nonce")
    args = parser.parse_args(argv)
    if args.execute and not all((args.owner, args.start_gate, args.start_nonce, args.capture_status, args.capture_nonce)):
        parser.error("Execution requires owned receipt, fresh driver ready/go and exact recorder status/nonce")
    if not args.execute and any((args.start_gate, args.start_nonce, args.capture_status, args.capture_nonce)):
        parser.error("Marker coordination is execution-only; default dry-run creates no controller")
    owner = h.load_owner(args.owner) if args.owner else None
    ledger_path = h.inside_build(args.ledger)
    ledger_path.parent.mkdir(parents=True, exist_ok=True)
    ledger = h.Ledger(ledger_path)
    guard = None
    try:
        ledger.write("coupled_virtual_session", scenario=args.scenario, execute=args.execute,
                     source_sha256=h.sha256(Path(__file__)), approved_driver_sha256=APPROVED_DRIVER_SHA,
                     timeline=[h.asdict(e) for e in scenarios(h)[args.scenario]], owner=h.asdict(owner) if owner else None,
                     boundary="Virtual request reports only; actual input consumption, trick/landing and visual quality remain separate")
        if args.execute:
            try:
                original = h.WindowsGuard(owner)
                guard = original  # Preserve metadata-handle cleanup if monitor setup refuses.
                monitor = MarkerMonitor(h, owner, args.capture_nonce, args.capture_status, windows_ticks())
                guard = TrialGuard(original, monitor)
                start_gate = coordinated_gate(h, monitor, args.start_gate, args.start_nonce, owner, args.scenario, h.sha256(Path(__file__)), ledger)
            except BaseException as error:
                ledger.write("controller_session_refused", completed=False, issue=f"{type(error).__name__}: {error}",
                             driver_report_count=0, driver_submitted=False, input_consumed=None, native_quality=None)
                raise
        else:
            guard, start_gate = h.DryGuard(), None
        result = h.run(scenarios(h)[args.scenario], guard, (lambda: h.make_vgamepad(args.dependency_receipt)) if args.execute else h.DryBackend,
                       ledger, execute=args.execute, clock=h.Clock() if args.execute else h.PreviewClock(), start_gate=start_gate)
        print(json.dumps(dict(ledger=str(ledger_path), **result)))
        return 0 if result["completed"] else 2
    finally:
        if guard is not None:
            guard.close()
        ledger.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"Coupled virtual test refused: {type(error).__name__}: {error}")
        raise SystemExit(2)
