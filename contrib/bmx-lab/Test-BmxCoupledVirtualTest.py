"""Portable fake-only tests: no Win32, controller imports, native API or game.

All fake profile/gate files are owned temporary children of ignored build/bmx.
The unchanged approved scheduler/bootstrap/ready-go functions run against a
Python fake backend and deterministic fake clocks, never installed ViGEm.
"""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("coupled_virtual_test", HERE / "Bmx-CoupledVirtualTest.py")
c = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = c
SPEC.loader.exec_module(c)
h = c.load_driver()  # Source-only import; vgamepad is not imported here.
FIXTURES = c.ROOT / "build/bmx/coupled-virtual-test-fixtures"
FIXTURES.mkdir(parents=True, exist_ok=True)


class Clock(h.PreviewClock):
    def __init__(self):
        super().__init__()
        self.action = None
        self.extra = 0

    def sleep(self, seconds):
        self.value += seconds + self.extra
        self.extra = 0
        if self.action:
            self.action()


class Guard:
    def __init__(self, action=None):
        self.calls, self.closed, self.action = 0, 0, action

    def check(self):
        self.calls += 1
        if self.action:
            self.action()

    def close(self):
        self.closed += 1


class Backend:
    def __init__(self, action=None, readback=None):
        self.states, self.closed, self.action, self.readback_override = [], 0, action, readback

    def prime_lsb(self):
        self.send(h.State(left_x=1 / 32767))

    def send(self, state):
        self.states.append(state)
        if self.action:
            self.action(state)

    def raw_report_hex(self):
        v = self.states[-1]
        return struct.pack("<HBBhhhh", v.buttons, round(v.left_trigger * 255), round(v.right_trigger * 255),
                           round(v.left_x * 32767), round(v.left_y * 32767), round(v.right_x * 32767), round(v.right_y * 32767)).hex()

    def readback(self):
        return self.readback_override or dict(available=True, slot=0, neutral=True, raw_report_hex="00" * 12)

    def close(self):
        self.closed += 1


class Sink:
    def __init__(self):
        self.rows = []

    def write(self, event, **fields):
        self.rows.append(dict(event=event, **fields))


def write(path, value):
    path.write_text(json.dumps(value, allow_nan=False), encoding="utf-8")


def epoch():
    return dict(owner_world=list(range(101, 109)), token=7, serial=100, time=1.5, h=c.NATIVE_H,
                bodies=[dict(world=106, index=11, generation=0), dict(world=106, index=12, generation=2)])


class Case(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="fake-", dir=FIXTURES)
        self.root = Path(self.tmp.name)
        self.old_root, h.WORKSPACE = h.WORKSPACE, self.root
        self.build = self.root / "build/bmx"
        self.build.mkdir(parents=True)
        self.profile = self.root / "build/bmx-user-data-coupled-test"
        self.profile.mkdir()
        self.owner = h.Owner(1234, 134054832620000000, str(self.root / "build/bmx-game/Skate.exe"), "A" * 64)
        self.nonce = "a" * 32
        self.status = self.build / "capture.status.json"
        self.clock, self.sink = Clock(), Sink()
        self.write_capture("waiting")
        self.monitor = c.MarkerMonitor(h, self.owner, self.nonce, self.status, self.ticks)
        self.guard = Guard()
        self.backend = Backend()

    def tearDown(self):
        h.WORKSPACE = self.old_root
        self.tmp.cleanup()

    def ticks(self):
        return 100000 + round(self.clock.now() * 1000)

    def write_capture(self, desired_state, **changes):
        value = dict(schema_version=1, event="bmx_recording_startup_gate", state=desired_state, nonce=self.nonce,
                     process_id=self.owner.process_id, process_creation_filetime=str(self.owner.process_creation_filetime),
                     observed_utc="2026-10-21T22:31:00Z", started_tick_ms=100000, elapsed_ms=0)
        value.update(changes)
        write(self.status, value)

    def activation(self, **changes):
        now = self.ticks()
        value = dict(schema=1, event="coupled_trial_activated", process_id=self.owner.process_id,
                     process_creation_filetime=str(self.owner.process_creation_filetime), capture_nonce=self.nonce,
                     operator_generation=100001, activation_begun_tick_ms=now, activation_succeeded_tick_ms=now,
                     conservative_expiry_tick_ms=now + 10000, epoch=epoch(), entry_ready=True,
                     actual_spring_render_verified=True, backend_active=True, normal_force_adoption_enabled=False)
        value.update(changes)
        return value

    def publish(self, **changes):
        self.write_capture("released")
        value = self.activation(**changes)
        write(self.monitor.activation, value)
        return value

    def terminal(self, **changes):
        value = dict(schema=1, event="coupled_trial_terminal", process_id=self.owner.process_id,
                     process_creation_filetime=str(self.owner.process_creation_filetime), capture_nonce=self.nonce,
                     operator_generation=100001, terminal_tick_ms=self.ticks(), issue="fake_explicit_terminal", epoch=epoch())
        value.update(changes)
        write(self.monitor.terminal, value)

    def gate(self):
        return c.coordinated_gate(h, self.monitor, self.build / "fresh-gate", "b" * 32,
                                  self.owner, "coupled-pedal-brake", "C" * 64, self.sink)

    def run_fake(self, *, gate=None, backend=None, guard=None, scenario="coupled-pedal-brake"):
        return h.run(c.scenarios(h)[scenario], c.TrialGuard(guard or self.guard, self.monitor),
                     lambda: backend or self.backend, self.sink, execute=True, clock=self.clock, start_gate=gate)

    def assert_cleanup(self, result, backend=None):
        backend = backend or self.backend
        self.assertTrue(result["neutral_release_succeeded"])
        self.assertTrue(result["disconnect_succeeded"])
        self.assertEqual(backend.states[-1], h.NEUTRAL)
        self.assertEqual(backend.closed, 1)
        self.assertIsNone(result["input_consumed"])
        self.assertIsNone(result["native_quality"])

    def test_baseline_pin_and_all_scenarios_are_untouched(self):
        self.assertEqual(h.sha256(c.ROOT / "contrib/bmx-lab/Bmx-VirtualGamepad.py"), c.APPROVED_DRIVER_SHA)
        self.assertEqual(h.PERIOD_MS, 20)
        self.assertEqual(h.POLL_MS, 10)
        self.assertEqual(h.MAX_LATE_MS, 120)
        self.assertEqual(h.MAX_DURATION_MS, 6000)
        self.assertEqual(h.START_GATE_LIMIT_MS, 60000)
        self.assertEqual(h.BOOTSTRAP_LIMIT_MS, 1000)
        before = copy.deepcopy(h.scenarios())
        new = c.scenarios(h)
        self.assertEqual(h.scenarios(), before)
        self.assertIn("native-y", before)
        self.assertNotIn("coupled-neutral", before)
        self.assertEqual(len(new), 4)
        for name, events in new.items():
            with self.subTest(scenario=name):
                h.validate_timeline(events)
                self.assertEqual(events[0], h.Event(0, h.NEUTRAL))
                self.assertEqual(events[-1], h.Event(6000, h.NEUTRAL))
                self.assertTrue(all(not e.state.buttons & (h.Y | 0x000F | 0x0030) for e in events))
                self.assertTrue(all(e.state.left_trigger == e.state.right_trigger == 0 for e in events))

    def test_baseline_mismatch_fails_before_execution(self):
        with patch.object(c, "APPROVED_DRIVER_SHA", "0" * 64), self.assertRaises(c.Refused):
            c.load_driver()
        self.assertNotIn("vgamepad", sys.modules)

    def test_marker_paths_require_exact_isolated_profile(self):
        self.assertEqual(self.monitor.activation.parent, self.profile)
        self.assertIn(str(self.owner.process_creation_filetime), self.monitor.activation.name)
        self.assertIn(self.nonce, self.monitor.activation.name)

    def test_bootstrap_ready_before_activation_then_301_reports(self):
        gate = self.gate()
        def producer():
            if gate.ready.exists() and not self.monitor.activation.exists():
                self.assertFalse(gate.go.exists())
                ready = json.loads(gate.ready.read_text())
                self.assertTrue(ready["neutral_bootstrap_completed"])
                self.assertEqual(ready["wait_limit_ms"], 60000)
                self.assertFalse(any(s.buttons for s in self.backend.states))
                self.publish()
        self.clock.action = producer
        result = self.run_fake(gate=gate)
        self.assertTrue(result["completed"], result)
        self.assertEqual(result["report_count"], 301)
        self.assert_cleanup(result)
        go = next(row for row in self.sink.rows if row["event"] == "coupled_activation_go")
        self.assertEqual(go["activation_sha256"], h.sha256(self.monitor.activation))
        self.assertEqual(go["runtime_sha256_from_owned_receipt"], self.owner.runtime_sha256)
        self.assertIsNone(go["input_consumed"])
        reports = [r for r in self.sink.rows if r["event"] == "controller_report"]
        self.assertEqual([r["planned_ms"] for r in reports if r["state"]["buttons"] == h.A], list(range(240, 1240, 20)))
        self.assertEqual([r["planned_ms"] for r in reports if r["state"]["buttons"] == h.B], list(range(2000, 2500, 20)))

    def test_no_marker_hits_unchanged_60_second_wait_and_cleanup(self):
        gate = self.gate()
        # Fake-only leap across idle polls: still exercise the original60s wall
        # deadline, without6,000 real filesystem traversals during a native hold.
        self.clock.action = lambda: setattr(self.clock, "value", min(60.02, self.clock.value + .99)) if gate.ready.exists() else None
        result = self.run_fake(gate=gate)
        self.assertFalse(result["completed"])
        self.assertIn("deadline", result["issue"])
        self.assertEqual(result["report_count"], 0)
        self.assertGreaterEqual(self.clock.now(), 60)
        self.assertLess(self.clock.now(), 60.04)
        self.assertFalse(gate.go.exists())
        self.assertFalse(any(s.buttons for s in self.backend.states))
        self.assert_cleanup(result)

    def test_external_go_cannot_bypass_missing_activation(self):
        gate = self.gate()
        def producer():
            if gate.ready.exists() and not gate.go.exists():
                write(gate.go, gate.trigger_fields())
        self.clock.action = producer
        result = self.run_fake(gate=gate)
        self.assertIn("External GO", result["issue"])
        self.assertEqual(result["report_count"], 0)
        self.assert_cleanup(result)

    def test_waiting_capture_cannot_authorize_go(self):
        self.clock.value = .2
        write(self.monitor.activation, self.activation())
        with self.assertRaisesRegex(c.Refused, "recorder gate"):
            self.monitor.ready()

    def test_activation_missing_required_flags_or_identity_refuses(self):
        self.clock.value = .2
        for changes in (dict(process_id=1235), dict(process_id=True), dict(process_creation_filetime="1"),
                        dict(capture_nonce="c" * 32), dict(operator_generation=0), dict(operator_generation=True),
                        dict(schema=True), dict(event="other"), dict(entry_ready=False), dict(backend_active=1),
                        dict(actual_spring_render_verified=False), dict(normal_force_adoption_enabled=True)):
            with self.subTest(changes=changes):
                self.publish(**changes)
                with self.assertRaises(c.Refused): self.monitor.ready()

    def test_epoch_strict_fields_finite_clocks_and_two_body_identity(self):
        mutations = [lambda v: v.update(token=0), lambda v: v.update(serial=True), lambda v: v.update(time=-1),
                     lambda v: v.update(time=float("nan")), lambda v: v.update(time=10**1000),
                     lambda v: v.update(h=.02), lambda v: v.update(h=True), lambda v: v.update(extra=0),
                     lambda v: v["owner_world"].__setitem__(0, 0), lambda v: v["owner_world"].__setitem__(1, True),
                     lambda v: v["bodies"][0].update(world=9), lambda v: v["bodies"][1].update(index=11),
                     lambda v: v["bodies"][0].update(index=1048576), lambda v: v["bodies"][0].update(generation=-1),
                     lambda v: v["bodies"][0].update(generation=True), lambda v: v["bodies"].pop()]
        self.assertEqual(c.validate_epoch(epoch()), epoch())
        for mutate in mutations:
            v = epoch(); mutate(v)
            with self.subTest(epoch=v), self.assertRaises(c.Refused): c.validate_epoch(v)

    def test_activation_exact_schema_no_extra_missing_or_duplicate_keys(self):
        self.clock.value = .2
        value = self.publish()
        for bad in (dict(value, extra=0), {k:v for k,v in value.items() if k != "epoch"}, []):
            write(self.monitor.activation, bad)
            with self.assertRaises(c.Refused): self.monitor.ready()
        raw = json.dumps(value)
        self.monitor.activation.write_text('{"schema":1,' + raw[1:], encoding="utf-8")
        with self.assertRaisesRegex(c.Refused, "Malformed"): self.monitor.ready()

    def test_activation_bad_json_oversize_or_nonfinite_refuses(self):
        for raw in (b"{", b"[]", b"x" * (c.MARKER_LIMIT + 1), b"", b'{"x":NaN}', b'\xff'):
            self.monitor.activation.write_bytes(raw)
            with self.subTest(raw=raw[:20]), self.assertRaises((c.Refused, ValueError)): self.monitor.ready()

    def test_freshness_and_conservative_budget_boundaries(self):
        self.clock.value = 2
        for changes in (dict(activation_begun_tick_ms=102001, activation_succeeded_tick_ms=102001, conservative_expiry_tick_ms=112001),
                        dict(activation_begun_tick_ms=100998, activation_succeeded_tick_ms=100998, conservative_expiry_tick_ms=110998),
                        dict(activation_begun_tick_ms=98000, activation_succeeded_tick_ms=102000, conservative_expiry_tick_ms=108000),
                        dict(conservative_expiry_tick_ms=112001), dict(operator_generation=102001)):
            self.publish(**changes)
            with self.subTest(changes=changes), self.assertRaises(c.Refused): self.monitor.ready()
        self.publish(activation_begun_tick_ms=101000, activation_succeeded_tick_ms=101000, conservative_expiry_tick_ms=111000)
        self.assertTrue(self.monitor.ready())  # Exactly1000ms fresh, >=6620ms remaining.

    def test_activation_cannot_predate_actual_recorder_release(self):
        self.clock.value = 2
        self.publish()
        self.write_capture("released", elapsed_ms=2001)
        with self.assertRaises(c.Refused): self.monitor.ready()

    def test_stale_mtime_and_preexisting_markers_refuse(self):
        self.clock.value = .2
        self.publish()
        os.utime(self.monitor.activation, ns=(1, 1))
        with self.assertRaises(c.Refused): self.monitor.ready()
        with self.assertRaisesRegex(c.Refused, "Preexisting"):
            c.MarkerMonitor(h, self.owner, self.nonce, self.status, self.ticks)
        self.monitor.activation.unlink()
        self.terminal()
        with self.assertRaisesRegex(c.Refused, "Preexisting"):
            c.MarkerMonitor(h, self.owner, self.nonce, self.status, self.ticks)

    def test_capture_initial_must_be_waiting_matching_owner_and_nonce(self):
        for changes in (dict(state="released"), dict(schema_version=True), dict(nonce="b" * 32),
                        dict(process_id=True), dict(process_creation_filetime="0134054832620000000"), dict(elapsed_ms=-1)):
            self.write_capture("waiting", **changes)
            with self.subTest(changes=changes), self.assertRaises(c.Refused):
                c.MarkerMonitor(h, self.owner, self.nonce, self.status, self.ticks)

    def test_terminal_before_activation_aborts_without_timeline(self):
        gate = self.gate()
        def producer():
            if gate.ready.exists() and not self.monitor.terminal.exists(): self.terminal(epoch=None)
        self.clock.action = producer
        result = self.run_fake(gate=gate)
        self.assertIn("Actual coupled terminal", result["issue"])
        self.assertEqual(result["report_count"], 0)
        self.assert_cleanup(result)

    def test_terminal_after_pedal_aborts_and_neutral_disconnects(self):
        gate = self.gate()
        self.clock.action = lambda: self.publish() if gate.ready.exists() and not self.monitor.activation.exists() else None
        self.backend.action = lambda s: self.terminal() if s.buttons == h.A and not self.monitor.terminal.exists() else None
        result = self.run_fake(gate=gate)
        self.assertIn("Actual coupled terminal", result["issue"])
        self.assertLess(result["report_count"], 301)
        self.assertFalse(any(s.buttons == h.B for s in self.backend.states))
        self.assertEqual(sum(s.buttons == h.A for s in self.backend.states), 1)
        self.assert_cleanup(result)

    def test_foreign_or_malformed_terminal_never_allows_more_input(self):
        self.clock.value = .2
        self.publish(); self.monitor.ready()
        for changes in (dict(operator_generation=100002), dict(process_id=9), dict(terminal_tick_ms=self.ticks()+1),
                        dict(issue=""), dict(epoch=dict(epoch(), token=9)), dict(epoch=dict(epoch(), h=.03))):
            self.terminal(**changes)
            with self.subTest(changes=changes), self.assertRaises(c.Refused): self.monitor.check()
        self.monitor.terminal.write_bytes(b"{")
        with self.assertRaises(c.Refused): self.monitor.check()

    def test_accepted_activation_tampering_or_disappearance_aborts(self):
        self.clock.value = .2
        original = self.publish()
        self.monitor.ready()
        write(self.monitor.activation, dict(original, operator_generation=100002))
        with self.assertRaisesRegex(c.Refused, "changed"): self.monitor.check()
        self.monitor.activation.unlink()
        with self.assertRaises(OSError): self.monitor.check()

    def test_actual_wall_clock_rewind_and_expiry_abort(self):
        self.clock.value = .2
        value = self.publish(); self.monitor.ready()
        self.clock.value = .199
        with self.assertRaisesRegex(c.Refused, "regressed"): self.monitor.check()
        self.clock.value = 10.2
        with self.assertRaisesRegex(c.Refused, "deadline"): self.monitor.check()

    def test_original_guard_remains_first_and_abort_cleanup_preserved(self):
        for reason in ("Owned process changed", "Owned game lost foreground", "Escape abort"):
            clock, backend, sink = Clock(), Backend(), Sink()
            def abort():
                if clock.now() > .5: raise h.Refused(reason)
            with self.subTest(reason=reason):
                guard = c.TrialGuard(Guard(abort), types.SimpleNamespace(check=lambda: None))
                result = h.run(c.scenarios(h)["coupled-pedal-brake"], guard, lambda: backend, sink, execute=True, clock=clock)
                self.assertIn(reason, result["issue"])
                self.assert_cleanup(result, backend)
        order = []
        original = Guard(lambda: order.append("original"))
        monitor = types.SimpleNamespace(check=lambda: order.append("monitor"))
        c.TrialGuard(original, monitor).check()
        self.assertEqual(order, ["original", "monitor"])

    def test_bootstrap_bad_readback_refuses_and_cleanup_runs(self):
        bad = Backend(readback=dict(available=True, slot=0, neutral=False, raw_report_hex="01" * 12))
        result = self.run_fake(gate=self.gate(), backend=bad)
        self.assertFalse(result["completed"])
        self.assertIn("Bootstrap", result["issue"])
        self.assertEqual(result["report_count"], 0)
        self.assert_cleanup(result, bad)

    def test_late_schedule_aborts_before_next_request_and_cleans_up(self):
        gate = self.gate()
        self.clock.action = lambda: self.publish() if gate.ready.exists() and not self.monitor.activation.exists() else None
        self.backend.action = lambda s: setattr(self.clock, "extra", .2) if s.buttons == h.A else None
        result = self.run_fake(gate=gate)
        self.assertIn("120ms", result["issue"])
        self.assertLess(result["report_count"], 301)
        self.assert_cleanup(result)

    def test_all_scenarios_fake_exact_six_seconds_and_301_reports(self):
        for name in c.scenarios(h):
            with self.subTest(scenario=name):
                clock, backend, sink = Clock(), Backend(), Sink()
                result = h.run(c.scenarios(h)[name], Guard(), lambda: backend, sink, execute=True, clock=clock)
                self.assertTrue(result["completed"], result)
                self.assertEqual(result["report_count"], 301)
                self.assert_cleanup(result, backend)

    def test_atomic_go_no_overwrite_or_foreign_temp_cleanup(self):
        go = self.build / "go.json"
        temporary = go.with_suffix(".tmp")
        temporary.write_text("other", encoding="utf-8")
        with self.assertRaises(FileExistsError): c.atomic_go(go, dict(a=1))
        self.assertEqual(temporary.read_text(), "other")
        self.assertFalse(go.exists())
        temporary.unlink()
        go.write_text("existing", encoding="utf-8")
        with self.assertRaises(FileExistsError): c.atomic_go(go, dict(a=1))
        self.assertEqual(go.read_text(), "existing")
        self.assertFalse(temporary.exists())
        go.unlink()
        c.atomic_go(go, dict(a=1))
        self.assertEqual(json.loads(go.read_text()), dict(a=1))
        self.assertFalse(temporary.exists())

    def test_no_live_api_or_import_is_needed_for_tests(self):
        self.assertNotIn("vgamepad", sys.modules)
        self.assertNotIn("vgamepad.win.vigem.client", sys.modules)

    def test_main_setup_refusal_closes_fake_guard_without_backend_construction(self):
        self.write_capture("released")
        owner_path, ledger = self.build / "owner.json", self.build / "ledger.jsonl"
        write(owner_path, dict(schema=1, event="bmx_virtual_gamepad_owner", process_id=self.owner.process_id,
                              process_creation_filetime=str(self.owner.process_creation_filetime), game_exe=self.owner.game_exe,
                              runtime_sha256=self.owner.runtime_sha256))
        metadata = Guard()
        def forbidden_backend(*unused):
            self.fail("Setup refusal must never construct a backend")
        with patch.object(c, "load_driver", return_value=h), patch.object(h, "WindowsGuard", return_value=metadata), \
                patch.object(h, "make_vgamepad", side_effect=forbidden_backend), patch.object(c, "windows_ticks", return_value=self.ticks):
            with self.assertRaises(c.Refused):
                c.main(["--execute", "--owner", str(owner_path), "--ledger", str(ledger), "--start-gate", str(self.build / "unused-gate"),
                        "--start-nonce", "b" * 32, "--capture-status", str(self.status), "--capture-nonce", self.nonce])
        self.assertEqual(metadata.closed, 1)
        rows = [json.loads(line) for line in ledger.read_text().splitlines()]
        self.assertEqual(rows[-1]["event"], "controller_session_refused")
        self.assertFalse(rows[-1]["driver_submitted"])
        self.assertEqual(rows[-1]["driver_report_count"], 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
