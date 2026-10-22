"""Encode genuine sampled @oai/sky JPEG/PNG window images with variable timing.

This never captures a screen, sends inputs, starts a game, releases a gate or
awards visual scores. Run with the bundled Python containing PyAV/Pillow/numpy.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
from fractions import Fraction
import hashlib
import json
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def inside(root: Path, candidate: Path) -> Path:
    resolved = candidate.resolve(strict=False)
    if not resolved.is_relative_to(root) or resolved == root:
        raise ValueError(f"Capture path escaped workspace: {candidate}")
    cursor = candidate.absolute()
    while cursor != root:
        if cursor.is_symlink() or (hasattr(cursor, "is_junction") and cursor.is_junction()):
            raise ValueError(f"Reparse path not supported: {cursor}")
        if cursor == cursor.parent:
            raise ValueError("Capture path escaped workspace")
        cursor = cursor.parent
    return resolved


def read_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def finalize(workspace: Path, manifest_path: Path, output: Path | None = None,
             run_json: Path | None = None, scenario: str | None = None,
             run_number: int | None = None, receipt_directory: Path | None = None):
    import av
    import numpy as np
    from PIL import Image

    workspace = workspace.resolve(strict=True)
    manifest_path = inside(workspace, manifest_path)
    manifest = read_json(manifest_path)
    if (manifest.get("schema") != 1 or manifest.get("event") != "bmx_sampled_live_window_capture"
            or manifest.get("completed") is not True or manifest.get("failure") is not None
            or manifest.get("sampled_live") is not True):
        raise ValueError("A completed, successful sampled-live capture manifest is required")
    samples = manifest.get("samples", [])
    if not 2 <= len(samples) <= 2000:
        raise ValueError("Expected 2..2000 captured frames")
    window = manifest["source_window"]
    owned = manifest["owned_process"]
    expected_app = "process:" + str(workspace / "build" / "bmx-game" / "Skate.exe")
    if window["app"].lower() != expected_app.lower():
        raise ValueError("Manifest does not identify the isolated Skate executable")
    times = []
    for index, sample in enumerate(samples):
        if (sample["index"] != index or sample["window_id"] != window["id"]
                or sample["app"] != window["app"]):
            raise ValueError("Capture window identity/index changed")
        started = sample["capture_started_monotonic_ms"]
        ended = sample["capture_ended_monotonic_ms"]
        midpoint = sample["presentation_monotonic_ms"]
        if not all(np.isfinite(value) for value in (started, ended, midpoint)) or not 0 <= started <= midpoint <= ended:
            raise ValueError("Capture interval is invalid")
        if index and (midpoint <= times[-1] or started < samples[index - 1]["capture_ended_monotonic_ms"]):
            raise ValueError("Capture intervals overlap or reverse")
        file = inside(workspace, manifest_path.parent / sample["file"])
        if file.parent != manifest_path.parent or file.suffix.lower() not in (".png", ".jpg", ".jpeg") or sha256(file) != sample["sha256"].upper():
            raise ValueError("Captured image path/hash mismatch")
        times.append(midpoint)
    width, height = samples[0]["width"], samples[0]["height"]
    if any((sample["width"], sample["height"]) != (width, height) for sample in samples):
        raise ValueError("Window dimensions changed; capture must be restarted")
    output = inside(workspace, output or manifest_path.parent / "sampled-live.mp4")
    audit_path = output.with_suffix(".validation.json")
    if output.exists() or audit_path.exists():
        raise ValueError("Preserve existing encoded evidence; output already exists")
    output.parent.mkdir(parents=True, exist_ok=True)
    pts = [round((time - times[0]) * 1000) for time in times]
    if any(right <= left for left, right in zip(pts, pts[1:])):
        raise ValueError("Measured presentation timestamps are not distinct")
    time_base = Fraction(1, 1_000_000)
    with av.open(str(output), "w") as container:
        container.metadata["comment"] = "Sampled live @oai/sky game-window recording. Variable PTS reflect capture intervals; not 60fps/native replay."
        stream = container.add_stream("libx264", rate=30)
        stream.width, stream.height = width, height
        stream.pix_fmt = "yuv420p" if width % 2 == 0 and height % 2 == 0 else "yuv444p"
        stream.time_base = time_base
        stream.codec_context.time_base = time_base
        stream.options = {"crf": "18", "preset": "veryfast", "bf": "0"}
        for sample, timestamp in zip(samples, pts):
            with Image.open(manifest_path.parent / sample["file"]) as image:
                if image.size != (width, height):
                    raise ValueError("Source image dimensions differ from capture manifest")
                frame = av.VideoFrame.from_ndarray(np.asarray(image.convert("RGB")), format="rgb24")
            frame.pts, frame.time_base = timestamp, time_base
            for packet in stream.encode(frame):
                container.mux(packet)
        for packet in stream.encode():
            container.mux(packet)
    decoded_times = []
    with av.open(str(output)) as container:
        if len(container.streams.video) != 1 or len(container.streams.audio) != 0:
            raise ValueError("Encoded capture must contain exactly one video stream and no fabricated audio")
        codec = container.streams.video[0].codec_context.name
        for frame in container.decode(video=0):
            if (frame.width, frame.height) != (width, height) or frame.pts is None:
                raise ValueError("Decoded capture dimensions/PTS are invalid")
            decoded_times.append(float(frame.pts * frame.time_base))
    expected_times = [timestamp / 1_000_000 for timestamp in pts]
    if len(decoded_times) != len(samples) or any(abs(actual - expected) > 0.000002 for actual, expected in zip(decoded_times, expected_times)):
        raise ValueError("Decoded frame count/variable timestamps differ from measured capture")
    gaps = [right - left for left, right in zip(times, times[1:])]
    audit = {
        "schema": 1, "event": "bmx_sampled_live_video_validation", "completed": True,
        "source_manifest": str(manifest_path), "source_manifest_sha256": sha256(manifest_path),
        "video_path": str(output), "video_sha256": sha256(output), "codec": codec,
        "width": width, "height": height, "frame_count": len(samples), "decoded_frame_count": len(decoded_times),
        "measured_sample_span_seconds": (times[-1] - times[0]) / 1000,
        "sample_rate_hz": (len(samples) - 1) * 1000 / (times[-1] - times[0]),
        "maximum_sample_gap_ms": max(gaps), "minimum_sample_gap_ms": min(gaps),
        "maximum_capture_latency_ms": max(sample["capture_ended_monotonic_ms"] - sample["capture_started_monotonic_ms"] for sample in samples),
        "variable_pts_verified": True, "sampled_live": True, "full_motion_60fps": False,
        "visual_pixels_reviewed": False, "visual_acceptance": False,
        "capture_started_utc": samples[0]["capture_ended_utc"],
        "capture_ended_utc": samples[-1]["capture_started_utc"],
        "boundary": "Genuine supplied JPEG/PNG window images, hash-checked and encoded with measured VFR timing. Low sampling can miss motion and brief defects. Pixel quality/scene content and native simulation timestamps require separate review.",
    }
    audit_path.write_text(json.dumps(audit, indent=2), encoding="utf-8")
    if receipt_directory is not None:
        if run_json is None or scenario is None or run_number is None:
            raise ValueError("Receipt generation needs --run-json, --scenario and --run-number")
        run = read_json(inside(workspace, run_json))
        if (run["owned_pid"] != owned["process_id"] or run["owned_created_utc"] != owned["process_created_utc"]
                or run["runtime_sha256"].upper() != owned["runtime_sha256"].upper()
                or Path(run["game_exe"]).resolve() != Path(owned["game_exe"]).resolve()):
            raise ValueError("Native run does not match the capture's external process identity")
        receipt_directory = inside(workspace, receipt_directory)
        receipt_directory.mkdir(parents=True, exist_ok=True)
        receipt_path = receipt_directory / f"{scenario}-{run_number}.capture.json"
        if receipt_path.exists():
            raise ValueError("Preserve existing receipt; choose a fresh run")
        receipt = {
            "schema": 1, "scenario": scenario, "run_number": run_number,
            "process_id": run["owned_pid"], "process_created_utc": run["owned_created_utc"],
            "runtime_sha256": run["runtime_sha256"], "game_exe": run["game_exe"],
            "capture_started_utc": audit["capture_started_utc"], "capture_ended_utc": audit["capture_ended_utc"],
            "capture_method": "@oai/sky sampled live game window; measured variable PTS",
            "capture_kind": "sampled_live", "video_path": str(output), "video_sha256": audit["video_sha256"],
            "source_manifest_path": str(manifest_path), "source_manifest_sha256": audit["source_manifest_sha256"],
            "validation_path": str(audit_path), "validation_sha256": sha256(audit_path),
            "sample_rate_hz": audit["sample_rate_hz"], "maximum_sample_gap_ms": audit["maximum_sample_gap_ms"],
            "full_motion_60fps": False, "visual_acceptance": False,
        }
        pending = receipt_path.with_suffix(".pending.json")
        pending.write_text(json.dumps(receipt, indent=2), encoding="utf-8")
        pending.rename(receipt_path)
        audit["receipt_path"] = str(receipt_path)
    return audit


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--run-json", type=Path)
    parser.add_argument("--scenario")
    parser.add_argument("--run-number", type=int)
    parser.add_argument("--receipt-directory", type=Path)
    arguments = parser.parse_args()
    print(json.dumps(finalize(arguments.workspace, arguments.manifest, arguments.output,
                             arguments.run_json, arguments.scenario, arguments.run_number,
                             arguments.receipt_directory), indent=2))
