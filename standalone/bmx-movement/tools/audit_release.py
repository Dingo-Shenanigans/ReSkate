"""Audit the v0.3.0 release against completed local evidence.

Runs the configured C++ tests and the separately installed consumer, reads recordings,
and writes VERIFICATION.json only after every required check succeeds. It never
operates a controller, launches the game, changes mods, or creates a ZIP.
"""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parents[1]
VERSION = "0.3.0"
REQUIRED_TESTS = {"bmx_tests", "bmx_pose_tests", "bmx_joint_actuation_tests", "bmx_vehicle_pose_tests",
                  "bmx_input_holdpoint_tests"}
NATIVE_RUNS = ("streets-reset-matrix01b",
               "streets-reset-matrix02", "streets-reset-matrix03",
               "streets-reset-matrix04", "streets-mc-griddle-retry01",
               "streets-controls-v33", "streets-air-v33",
               "streets-visual-retry-v33", "streets-suspension-v33")
VISUAL_ONLY = "streets-visual-matrix01"
GAME_HASH = "22a4ac9cbec7d270e96dcc90b9870f69f91596dd974fbb1c874b1b5e081a94b8"
META_HASH = "7e1af5100b90a78c907999239bc6952dfdce2461fc4321626658f24f2815d7ea"


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def child(parent, value):
    value = str(value)
    require(Path(value).name == value and value not in ("", ".", "..")
            and "/" not in value and "\\" not in value, "Expected one child basename: " + value)
    return parent / value


def source_path(value):
    path = Path(str(value).replace("\\", "/"))
    if not path.is_absolute():
        if (path.parts[:2] == ("standalone", "bmx-movement")
                or len(path.parts) >= 2 and path.parts[0] == "build" and path.parts[1].startswith("bmx-")):
            path = WORKSPACE / path
        else:
            path = ROOT / path
    path = path.resolve(strict=True)
    require(path.is_relative_to(WORKSPACE), "Evidence source escaped workspace: " + str(path))
    return path


def binding(path):
    path = Path(path).resolve(strict=True)
    return {"path": str(path), "bytes": path.stat().st_size, "sha256": digest(path)}


def run_program(path):
    completed = subprocess.run([str(Path(path).resolve(strict=True))],
                               check=True, capture_output=True, text=True, timeout=120)
    return completed.stdout + completed.stderr


def archive_members(path):
    """Read resident ar payloads without trusting nonsemantic header dates."""
    data = Path(path).read_bytes()
    require(data[:8] == b"!<arch>\n", "Expected a resident ar library: " + str(path))
    offset = 8
    members = []
    while offset < len(data):
        require(offset + 60 <= len(data), "Truncated ar member header: " + str(path))
        header = data[offset:offset + 60]
        require(header[58:60] == b"`\n", "Invalid ar member header: " + str(path))
        name = header[:16].decode("ascii").rstrip(" ")
        size_text = header[48:58].decode("ascii").strip()
        require(size_text.isdecimal(), "Invalid ar member length: " + str(path))
        size = int(size_text)
        start = offset + 60
        end = start + size
        require(end <= len(data), "Truncated ar member payload: " + str(path))
        members.append({"name": name, "bytes": size,
                        "mode": header[40:48].decode("ascii").strip(),
                        "sha256": hashlib.sha256(data[start:end]).hexdigest()})
        offset = end
        if size % 2:
            require(offset < len(data) and data[offset:offset + 1] == b"\n",
                    "Invalid ar member alignment: " + str(path))
            offset += 1
    require(members and offset == len(data), "Empty or malformed ar library: " + str(path))
    return members


def driver(session, name):
    path = child(session, name + ".driver.json")
    value = read(path)
    require(value.get("completed") is True and value.get("failure") is None, "Driver failed: " + name)
    require(value.get("driver_report_count") == value.get("matching_driver_reports")
            and value.get("matching_driver_reports", 0) > 0, "Driver readback mismatch: " + name)
    for key in ("neutral_release_succeeded", "disconnect_succeeded", "disconnect_readback_proven"):
        require(value.get(key) is True, "Driver cleanup missing: " + name + "/" + key)
    require(value.get("post_disconnect_xinput_error") == 1167, "Controller stayed connected: " + name)
    require(value.get("timeline_sha256") and value.get("timeline_start_utc"),
            "Driver timeline identity missing: " + name)
    return value, binding(path)


def scan_fixed(path, valid_field=False, index_field="fixed_index"):
    """Check every index/presence prefix; fully parse first and last records.

    Complete file hashes bind the remaining serialized values. This intentionally
    does not claim semantic verification of every nested body/animation value.
    """
    count = valid = invalid = 0
    first = last = first_valid = None
    prefix = re.compile(rb'\{"' + index_field.encode("ascii") + rb'":\s*(\d+),')
    with path.open("rb") as stream:
        for line in stream:
            require(line.endswith(b"\n"), "Incomplete JSONL tail: " + str(path))
            match = prefix.match(line[:100])
            require(match is not None and int(match[1]) == count, "Fixed index gap: " + str(path))
            if valid_field:
                state = re.search(rb'"sample_valid":\s*(true|false)', line[:700])
                require(state is not None, "Missing availability marker: " + str(path))
                valid += state[1] == b"true"
                invalid += state[1] == b"false"
                if state[1] == b"true" and first_valid is None:
                    first_valid = json.loads(line)
            if first is None:
                first = json.loads(line)
            last = line
            count += 1
    require(count > 0 and first is not None, "Empty fixed channel: " + str(path))
    json.loads(last)
    return {"rows": count, "valid_rows": valid if valid_field else None,
            "invalid_rows": invalid if valid_field else None, "index_gaps": 0,
            "first_and_last_full_json_valid": True, "first_record": first,
            "first_valid_record": first_valid,
            "file": binding(path)}


def frames(directory, summary):
    path = directory / "frames.csv"
    with path.open(encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.DictReader(stream))
    require(len(rows) == summary.get("frames") and len(rows) > 1,
            "Frame ledger count mismatch: " + str(directory))
    require(all(int(row["index"]) == i for i, row in enumerate(rows)), "Frame index gap")
    return binding(path)


def native_run(session, name):
    directory = child(session, name)
    proof, proof_file = driver(session, name)
    require(proof.get("target") == "streets", "Native driver target mismatch: " + name)
    summary = read(directory / "capture-summary.json")
    require(summary.get("completed") is True and summary.get("failure") is None, "Incomplete native run: " + name)
    require(summary.get("rendered_live") is True and summary.get("window_capture") is False
            and summary.get("scripted") is False, "Native framebuffer origin missing: " + name)
    require(summary.get("errors") == 0 and not summary.get("channel_errors"),
            "Native channel errors: " + name)
    if "v33" in name:
        start = read(directory / "capture-start.json")
        require(start.get("all_channels_opened") is True and start.get("schema", 0) >= 2,
                "Transactional V3.3 startup not confirmed: " + name)
    body = scan_fixed(directory / "body-physics.jsonl", True)
    trick = scan_fixed(directory / "trick-states.jsonl")
    pose = scan_fixed(directory / "rider.jsonl", index_field="index")
    require(body["rows"] == trick["rows"] == summary.get("physics_samples")
            == summary.get("telemetry_ticks"), "Native fixed channel counts differ: " + name)
    require(body["valid_rows"] == summary.get("valid_fixed_ticks")
            and body["invalid_rows"] == summary.get("invalid_fixed_ticks"), "Native validity counts differ: " + name)
    exemplar = body["first_valid_record"]
    require(exemplar is not None and len(exemplar.get("bodies", [])) == 8
            and all(b.get("available") is True for b in exemplar["bodies"])
            and exemplar.get("drive_connection_joint") is not None,
            "Selected eight-body/joint exemplar incomplete: " + name)
    require(pose["rows"] == summary.get("bone_samples"), "Native pose count mismatch: " + name)
    require((directory / "channel-errors.jsonl").stat().st_size == 0, "Native error ledger nonempty: " + name)
    body.pop("first_record")
    body.pop("first_valid_record")
    trick.pop("first_record")
    trick.pop("first_valid_record")
    pose.pop("first_record")
    pose.pop("first_valid_record")
    return {"name": name, "target": "streets", "scenario": proof["scenario"],
            "timeline_sha256": proof["timeline_sha256"], "driver": proof_file,
            "capture": binding(directory / "capture-summary.json"),
            "frame_ledger": frames(directory, summary), "frames": summary["frames"],
            "body_channel": body, "trick_channel": trick, "pose_channel": pose, "cleanup_verified": True,
            "missing_vehicle_rows_are_explicit": True}


def visual_only_run(session):
    directory = session / VISUAL_ONLY
    proof, proof_file = driver(session, VISUAL_ONLY)
    require(proof.get("target") == "streets", "Visual-only driver target mismatch")
    summary = read(directory / "capture-summary.json")
    limitation = read(directory / "capture-channel-limitation.json")
    require(summary.get("completed") is True and summary.get("rendered_live") is True
            and summary.get("window_capture") is False and summary.get("scripted") is False, "Visual-only source incomplete")
    require(summary.get("physics_samples") == 0 and summary.get("errors", 0) > 0,
            "V3.2 gap no longer matches quarantine receipt")
    require(limitation.get("physics_coverage_valid") is False, "Visual-only physics gap not explicit")
    require(not (directory / "body-physics.jsonl").exists()
            and not (directory / "trick-states.jsonl").exists(), "Unexpected V3.2 fixed channel files")
    require((directory / "rider.jsonl").stat().st_size > 0 and summary.get("bone_samples", 0) > 0, "Visual-only pose stream missing")
    return {"name": VISUAL_ONLY, "target": "streets", "scenario": proof["scenario"],
            "timeline_sha256": proof["timeline_sha256"],
            "role": "video_pose_ik_only", "physics_coverage_valid": False,
            "fixed_physics_claims_quarantined": True, "frames": summary["frames"],
            "bone_samples": summary["bone_samples"], "driver": proof_file,
            "capture": binding(directory / "capture-summary.json"),
            "limitation": binding(directory / "capture-channel-limitation.json"),
            "frame_ledger": frames(directory, summary), "cleanup_verified": True}


def flick_provenance(session, native_runs):
    """Bind generated flick commands to the two audited native recordings."""
    policy_path = ROOT / "reference/flick-hop-policy-v2.json"
    policy = read(policy_path)
    require(policy["source_identity"]["GameAssembly_sha256"] == GAME_HASH
            and policy["source_identity"]["global_metadata_sha256"] == META_HASH,
            "Flick policy refers to another native build")
    provenance_path = ROOT / "reference/observed-joint-actuation-tracks.provenance.json"
    provenance = read(provenance_path)
    generated = source_path(provenance["generated_cpp"])
    require(digest(generated) == provenance["generated_sha256"], "Generated joint command provenance is stale")
    require(digest(ROOT / "include/bmx/joint_actuation.hpp") == provenance["header_sha256"]
            and digest(ROOT / "tools/export_joint_cpp.py") == provenance["exporter_sha256"],
            "Joint command exporter/header provenance is stale")
    tracks = provenance["tracks"]
    phases = [track for track in tracks if track["id"] == 83]
    require(len(tracks) == 113 and len(phases) == 3
            and sorted(track["phase"] for track in phases) == ["enter", "recover", "transition"],
            "Expected the three separately recorded preload/flick/recovery phases")
    source = session / "analysis-suspension-v33/flick-joint-native-v2.json"
    require(all(source_path(track["selected_source"]) == source.resolve()
                and track["generated_keys"] > 1 for track in phases), "Flick phase source differs")
    sources = [entry for entry in provenance["sources"] if source_path(entry["path"]) == source.resolve()]
    require(len(sources) == 1 and digest(source) == sources[0]["sha256"], "Flick source bundle changed")
    value = read(source)
    native = {run["name"]: run for run in native_runs}
    suspension, controls = native["streets-suspension-v33"], native["streets-controls-v33"]
    require(value["source_body_physics_sha256"] == suspension["body_channel"]["file"]["sha256"]
            and value["driver_sha256"] == suspension["driver"]["sha256"],
            "Flick fit no longer matches the audited suspension capture")
    recovery = value["independent_recovery_source"]
    require(recovery["body_physics_sha256"] == controls["body_channel"]["file"]["sha256"]
            and recovery["driver_sha256"] == controls["driver"]["sha256"],
            "Flick cancellation no longer matches its separate audited control capture")
    require(all(Path(track["phase_source"].replace("\\", "/")).name ==
                ("streets-controls-v33" if track["phase"] == "recover" else "streets-suspension-v33")
                for track in phases), "Flick phase provenance conflates cancellation with launch")
    validation_path = source.parent / "flick-export-validation.json"
    validation = read(validation_path)
    require(validation["generated_sha256"] == provenance["generated_sha256"]
            and validation["old_track_count"] == 110 and validation["new_track_count"] == 113
            and validation["old_key_arrays_exact_prefix"] is True
            and validation["old_descriptors_exact"] is True,
            "Flick generation validation is absent or stale")
    return {"policy": binding(policy_path), "command_provenance": binding(provenance_path),
            "generated_cpp": binding(generated), "compact_export_validation": binding(validation_path),
            "source_bundle": binding(source), "actuation_phases": sorted(track["phase"] for track in phases),
            "generated_track_count": len(tracks), "source_capture_hashes_match_audited_runs": True,
            "limits": ["This checks source/export bindings; it does not establish unmeasured Nollie animation or full physics parity.",
                       "The raw command bundle remains in the workspace and is excluded from distribution."]}


def coverage(session):
    path = session / "coverage-complete-primary.json"
    value = read(path)
    slots = value["slots"]
    require(len(slots) == 64 and {(s["set"], s["slot"]) for s in slots}
            == {(a, b) for a in range(8) for b in range(8)}, "Catalog does not contain 64 unique positions")
    available = [s for s in slots if s["available"]]
    empty = [s for s in slots if not s["available"]]
    require(len(available) == 58 and len(empty) == 6, "Current catalog availability differs")
    for slot in available:
        best = slot.get("best_attempt") or {}
        require(all(best.get(k) is True for k in ("fire_observed", "native_exit_to_idle_observed",
                                                 "full_posture_window_observed", "source_complete_through_attempt")),
                "Incomplete native selection observation: " + str((slot["set"], slot["slot"])))
        require(any(int(f["set"]) == slot["set"] and int(f["slot"]) == slot["slot"]
                    for f in best.get("fire_observations", [])), "Wrong actual fire identity")
    require(all(s.get("empty_control_verified_no_fire") is True for s in empty), "Empty control fired or untested")
    catalog = read(ROOT / "reference/current-trick-catalog-timing-v3.json")["catalog_slots"]
    require({(s["set"], s["slot"], s["name"]) for s in catalog}
            == {(s["set"], s["slot"], s["name"]) for s in available}, "Canonical catalog timing table differs")
    sources = []
    for item in value["sources"]:
        for field in ("ledger", "posture"):
            path0 = source_path(item[field])
            require(digest(path0) == item[field + "_sha256"], "Changed coverage source: " + str(path0))
            sources.append(binding(path0))
    return {"ledger": binding(path), "available": 58, "empty": 6, "correct_native_fire": 58,
            "native_exit_to_idle": 58, "full_posture_window": 58, "empty_no_fire": 6,
            "native_scored_landing_proven": False, "sources": sources,
            "limits": value.get("limits", [])}


def coupled_convergence(directory, expected_ticks):
    path = directory / "coupled-physics.jsonl"
    rows = valid = converged = 0
    maximum_residual = 0.0
    with path.open(encoding="utf-8") as stream:
        for line in stream:
            require(line.endswith("\n"), "Incomplete coupled-physics tail: " + str(path))
            record = json.loads(line)
            rows += 1
            require(record.get("valid") is True, "Invalid coupled physics row: " + str(path))
            valid += 1
            require(record.get("converged") is True, "Unconverged coupled physics row: " + str(path))
            converged += 1
            residual = record.get("coupling_residual")
            require(isinstance(residual, (int, float)) and not isinstance(residual, bool)
                    and math.isfinite(residual) and residual >= 0,
                    "Invalid coupled residual: " + str(path))
            maximum_residual = max(maximum_residual, residual)
    require(rows == expected_ticks and rows > 0, "Coupled physics tick count differs: " + str(path))
    return {"file": binding(path), "rows": rows, "valid_rows": valid,
            "converged_rows": converged, "nonconverged_rows": 0,
            "maximum_coupling_residual": maximum_residual,
            "all_valid_ticks_converged": True}


def demo_run(session, name, expected_binary):
    proof, proof_file = driver(session, name)
    require(proof.get("target") == "demo" and proof.get("executable_sha256") == expected_binary,
            "Final demo driver used another executable: " + name)
    summary = read(session / (name + ".csv.summary.json"))
    require(summary.get("invalid_ticks") == 0 and summary.get("xinput_ticks", 0) > 0,
            "Final demo simulation invalid or missing XInput: " + name)
    capture_path = session / name / "capture-summary.json"
    capture = read(capture_path)
    require(capture.get("completed") is True and capture.get("rendered_live") is True
            and capture.get("window_capture") is False and capture.get("scripted") is False,
            "Final demo lacks actual framebuffer capture: " + name)
    require(capture.get("invalid_ticks", capture.get("errors", 0)) == 0, "Final demo capture error")
    convergence = coupled_convergence(session / name, summary["ticks"])
    return {"name": name, "target": "demo", "scenario": proof["scenario"],
            "timeline_sha256": proof["timeline_sha256"], "executable_sha256": expected_binary,
            "driver": proof_file, "capture": binding(capture_path),
            "simulation_summary": binding(session / (name + ".csv.summary.json")),
            "frames": capture["frames"], "frame_ledger": frames(session / name, capture),
            "physics_ticks": summary["ticks"], "xinput_ticks": summary["xinput_ticks"],
            "takeoffs": summary.get("takeoffs"), "landed_combos": summary.get("landed_combos"),
            "coupled_solver": convergence,
            "cleanup_verified": True}


def supplementary_reports(session, final_runs):
    matches = [re.search(r"-(v2[a-z]+)$", run["name"]) for run in final_runs]
    require(matches and all(matches), "Final demos need a common v2 capture suffix")
    tags = {match[1] for match in matches}
    require(len(tags) == 1, "Final demos mix capture suffixes")
    tag = tags.pop()
    names = ("actual-capture-quality-" + tag + ".json",
             "actual-controls-hop-audit-" + tag + ".json", "session-manifest.json")
    files = []
    for name in names:
        path = session / name
        require(0 < path.stat().st_size <= 16 * 1024 * 1024,
                "Supplementary report is empty or exceeds compact metadata limit: " + name)
        require(isinstance(read(path), dict), "Supplementary report must be a JSON object: " + name)
        files.append(binding(path))
    return {"capture_tag": tag, "files": files}


def installed_consumer(path):
    path = path.resolve(strict=True)
    value = read(path)
    require(value.get("schema") == 1 and value.get("completed") is True
            and value.get("configured_version") == VERSION and value.get("exit_code") == 0,
            "Installed consumer receipt incomplete")
    require(Path(value["source_root"]).resolve() == ROOT.resolve(), "Consumer was built from another source tree")
    prefix = Path(value["install_prefix"]).resolve(strict=True)
    library = prefix / "lib/libbmx_movement.a"
    require(digest(library) == value["library_sha256"], "Installed archive changed")
    current_library = ROOT / "build/libbmx_movement.a"
    installed_members = archive_members(library)
    require(installed_members == archive_members(current_library),
            "Installed/current archive member payloads or order differ; rebuild both from the frozen sources with the same compiler/configuration")
    executable = Path(value["consumer_executable"]).resolve(strict=True)
    require(digest(executable) == value["consumer_sha256"], "Installed consumer executable changed")
    headers = value["installed_headers"]
    require(len(headers) > 0, "No installed header provenance")
    for header in headers:
        installed = Path(header["path"]).resolve(strict=True)
        relative = installed.relative_to(prefix / "include")
        require(digest(installed) == header["sha256"] == digest(ROOT / "include" / relative),
                "Installed/current header mismatch: " + str(relative))
    expected = {p.relative_to(ROOT / "include").as_posix() for p in (ROOT / "include").rglob("*") if p.is_file()}
    actual = {Path(h["path"]).resolve().relative_to(prefix / "include").as_posix() for h in headers}
    require(actual == expected, "Installed header coverage incomplete")
    output = run_program(executable)
    return {"receipt": binding(path), "installed_library": binding(library),
            "current_build_library": binding(current_library),
            "consumer": binding(executable), "all_installed_headers_match_current": True,
            "installed_archive_members_match_current_build": True,
            "installed_archive_byte_identical_to_current_build": digest(library) == digest(current_library),
            "archive_member_comparison": {"format": "ar", "members": installed_members,
                                          "ignored_header_fields": ["timestamp", "uid", "gid"],
                                          "special_index_and_long_names_payloads_compared": True},
            "consumer_rerun_exit_code": 0, "consumer_output": output,
            "configured_version": VERSION}


def headless_validation(session):
    path = session / "headless-release-validation.json"
    value = read(path)
    require(value.get("event") == "frozen_release_headless_validation"
            and value.get("actual_controller_recording") is False,
            "Headless release report origin differs")
    counts = value["counts"]
    require(counts.get("attempted") == 58 and counts.get("adopted") == 58
            and counts.get("invalid_samples") == 0, "Headless release selection/validity coverage differs")
    sources = []
    has_current_library = False
    for item in value["sources"]:
        source = source_path(item["path"])
        require(digest(source) == item["sha256"], "Headless release source changed: " + str(source))
        has_current_library |= source == (ROOT / "build/libbmx_movement.a").resolve()
        sources.append(binding(source))
    require(has_current_library, "Headless report does not bind the current library")
    return {"report": binding(path), "sources": sources, "counts": counts,
            "actual_controller_recording": False, "all_source_hashes_match_current": True}


def comparison(session, directory, paired_drivers, quarantined_driver):
    directory = directory.resolve(strict=True)
    require(directory.is_relative_to(session), "Comparison must remain in selected session")
    record = read(directory / "comparison.json")
    validation = read(directory / "comparison-video.validation.json")
    require(record.get("source_driver_hashes") in paired_drivers, "Comparison references other driver receipts")
    if record["source_driver_hashes"]["streets"] == quarantined_driver:
        require(record.get("metrics", {}).get("physics_comparison_available") is False
                and record.get("streets_source", {}).get("fixed_physics_comparison_available") is False,
                "V3.2 visual-only source used for a fixed physics comparison")
    require(validation.get("container_and_frame_bytes_verified") is True
            and validation.get("resized_actual_source_frames") is True
            and validation.get("generated_motion") is False, "Comparison video lacks actual frame verification")
    video = directory / "comparison.avi"
    require(digest(video) == validation["sha256"], "Comparison video hash mismatch")
    result = {"directory": directory.relative_to(ROOT).as_posix(),
              "report": binding(directory / "comparison.json"),
              "validation": binding(directory / "comparison-video.validation.json"),
              "video": binding(video), "source_driver_hashes": record["source_driver_hashes"]}
    if validation.get("gif"):
        gif = directory / "comparison.gif"
        require(digest(gif) == validation["gif"]["sha256"], "Comparison GIF changed")
        result["gif"] = binding(gif)
    return result


def main(args):
    session = child(ROOT / "recordings", args.session).resolve(strict=True)
    require(re.search(r"project\(bmx_movement VERSION " + re.escape(VERSION), (ROOT / "CMakeLists.txt").read_text()),
            "CMake release version differs")
    configured = re.findall(r"add_executable\((bmx_[A-Za-z0-9_]*tests)\s+tests/",
                            (ROOT / "CMakeLists.txt").read_text())
    require(REQUIRED_TESTS.issubset(configured) and len(configured) == len(set(configured)),
            "Required CMake test targets missing or duplicated")
    test_programs = [name + ".exe" for name in configured]
    tests = []
    for name in test_programs:
        executable = ROOT / "build" / name
        output = run_program(executable)
        matches = re.findall(r"(\d+)\s+(?:[A-Za-z ]*\s)?assertions", output)
        require(matches and int(matches[-1]) > 0, "No assertion result from " + name)
        tests.append({"executable": binding(executable), "exit_code": 0,
                      "assertions_passed": int(matches[-1]), "output": output})
    for item in read(ROOT / "source-manifest.json")["files"]:
        require(digest(ROOT / "include/bmx/detail/source" / item["file"]) == item["bundled_sha256"],
                "Bundled source manifest changed: " + item["file"])
    native = [native_run(session, n) for n in NATIVE_RUNS]
    flick = flick_provenance(session, native)
    quarantine = visual_only_run(session)
    available = coverage(session)
    binary = digest(ROOT / "build/bmx_demo.exe")
    demos = [demo_run(session, n, binary) for n in args.final_demo]
    supplementary = supplementary_reports(session, demos)
    all_runs = {r["name"]: r for r in native + demos + [quarantine]}
    pairs = []
    pair_hashes = []
    for item in args.matched:
        left, right = item.split("=", 1)
        require(left in all_runs and right in all_runs, "Matched run was not audited: " + item)
        a, b = all_runs[left], all_runs[right]
        require(a["target"] == "streets" and b["target"] == "demo"
                and a["scenario"] == b["scenario"] and a["timeline_sha256"] == b["timeline_sha256"],
                "Matched requested timelines differ: " + item)
        pair_hashes.append({"streets": a["driver"]["sha256"], "demo": b["driver"]["sha256"]})
        pairs.append({"streets": left, "demo": right, "timeline_sha256": a["timeline_sha256"],
                      "native_fixed_physics_available": left != VISUAL_ONLY})
    require(pairs, "At least one verified matched native/demo pair is required")
    comparisons = [comparison(session, Path(p), pair_hashes, quarantine["driver"]["sha256"])
                   for p in args.comparison]
    require(comparisons, "At least one actual two-target comparison video is required")
    consumer = installed_consumer(Path(args.installed_consumer_receipt))
    headless = headless_validation(session)
    deployment_path = session / "deployment.json"
    deployment = read(deployment_path)
    require(deployment.get("Restored") is True, "Game restoration receipt missing")
    local_verified = False
    if args.local_install:
        require(digest(Path(deployment["CatModOriginal"])) == deployment["CatModSha256"].lower(), "CatMod not restored")
        require(not Path(deployment["CatModStaged"]).exists() and not Path(deployment["Observer"]).exists(),
                "Temporary recorder/mod staging still present")
        game = Path(deployment["GameRoot"])
        require(digest(game / "GameAssembly.dll") == GAME_HASH, "Installed native build changed")
        require(digest(game / "BMX Streets_Data/il2cpp_data/Metadata/global-metadata.dat") == META_HASH,
                "Installed metadata changed")
        local_verified = True
    require(local_verified, "Final release audit requires --local-install")
    verified = {}
    for directory in ("include", "src", "tests", "demo", "reference"):
        for file in (ROOT / directory).rglob("*"):
            if file.is_file():
                verified[file.relative_to(ROOT).as_posix()] = digest(file)
    for name in ("CMakeLists.txt", "source-manifest.json", "README.md", "REFERENCE.md", "COMPARISON.md", "LICENSE"):
        verified[name] = digest(ROOT / name)
    for name in ("bmx_demo.exe", *test_programs, "libbmx_movement.a"):
        verified["build/" + name] = digest(ROOT / "build" / name)
    result = {"schema": 3, "version": VERSION, "date": "2026-10-21",
              "event": "bmx_standalone_release_audit", "completed": True,
              "session": session.relative_to(ROOT).as_posix(), "cxx_standard": 20,
              "tests": tests, "assertions_passed": sum(t["assertions_passed"] for t in tests),
              "current_native_catalog_coverage": available,
              "healthy_native_runs": native, "visual_only_quarantine": quarantine,
              "flick_hop_provenance": flick,
              "final_demo_runs": demos, "matched_requested_timelines": pairs,
              "supplementary_reports": supplementary,
              "comparisons": comparisons, "installed_consumer": consumer,
              "headless_release_validation": headless,
              "source_manifest_all_bundled_hashes_verified": True,
              "deployment": binding(deployment_path),
              "local_install_restoration_and_game_identity_verified": local_verified,
              "physical_controller_tested": False,
              "actual_input": "Installed ViGEm virtual Xbox controller / XInput",
              "native_scored_landing_proven": False,
              "full_native_physics_or_animation_parity_verified": False,
              "raw_native_streams_in_distribution": False,
              "fresh_zip_extracted_build_verified": False,
              "verified_files": verified,
              "limits": [
                  "58 actual selections, phase completion and posture windows are observed; native scored landing is not established.",
                  "V3.2 full matrix is video/pose/IK only: its missing fixed channels are quarantined.",
                  "Every fixed-row index and availability prefix is checked; first/last nested JSON are parsed. All nested values are not independently semantically verified.",
                  "Raw frames and large JSONL/trajectory inputs remain in the workspace; derived C++ tracks, hashes and reports are portable.",
                  "The coupled model is a reduced reconstruction; solver, contacts, some animation/PID controls, ragdoll and walking are not full native parity.",
                  "Actual frame samples can be repeated for video presentation; no generated intermediate motion or desktop recording is claimed.",
                  "The installed consumer is checked before packaging; a fresh ZIP extraction/build is a separate later receipt.",
                  "Windows/MinGW and virtual XInput were exercised; other platforms/engines and a physical controller were not."
              ]}
    output = ROOT / "VERIFICATION.json"
    output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"verification": str(output), "version": VERSION, "assertions": result["assertions_passed"],
                      "native_runs": len(native), "final_demos": len(demos), "comparisons": len(comparisons),
                      "catalog_available": 58, "catalog_empty": 6}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--session", default="comparison-session02")
    parser.add_argument("--final-demo", action="append", required=True, help="Fresh final run basename; repeat for all final runs")
    parser.add_argument("--matched", action="append", required=True, metavar="NATIVE=DEMO")
    parser.add_argument("--comparison", action="append", required=True, help="Completed comparison output folder")
    parser.add_argument("--installed-consumer-receipt", required=True)
    parser.add_argument("--local-install", action="store_true")
    main(parser.parse_args())
