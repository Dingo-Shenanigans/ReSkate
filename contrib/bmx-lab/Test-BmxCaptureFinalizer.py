"""Synthetic offline encoding fixtures; these are never game footage."""
import importlib.util
import json
from pathlib import Path
import shutil
import unittest
import uuid

from PIL import Image

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("bmx_finalizer", HERE / "Finalize-BmxCapture.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class FinalizerTests(unittest.TestCase):
    def setUp(self):
        self.workspace = HERE.parent.parent
        self.directory = self.workspace / "build" / "bmx" / "sampled-capture-fixtures" / str(uuid.uuid4())
        self.directory.mkdir(parents=True)
        self.manifest_path = self.directory / "capture-manifest.json"
        self.manifest = {
            "schema": 1, "event": "bmx_sampled_live_window_capture", "completed": True,
            "failure": None, "sampled_live": True,
            "source_window": {"app": "process:" + str(self.workspace / "build" / "bmx-game" / "Skate.exe"), "id": 123},
            "owned_process": {"process_id": 12345}, "samples": [],
        }
        for index, time in enumerate((0, 1200, 3100)):
            file = self.directory / f"frame-{index:06}.png"
            Image.new("RGB", (64, 48), (30 + index * 90, 60, 100)).save(file)
            self.manifest["samples"].append({
                "index": index, "file": file.name, "sha256": module.sha256(file), "window_id": 123,
                "app": self.manifest["source_window"]["app"], "width": 64, "height": 48,
                "capture_started_monotonic_ms": time, "capture_ended_monotonic_ms": time + 10,
                "presentation_monotonic_ms": time + 5,
                "capture_started_utc": f"2026-10-19T12:00:0{index}Z", "capture_ended_utc": f"2026-10-19T12:00:0{index}Z",
            })
        self.save()

    def save(self):
        self.manifest_path.write_text(json.dumps(self.manifest), encoding="utf-8")

    def tearDown(self):
        checked = self.directory.resolve()
        self.assertEqual(checked.parent, self.workspace / "build" / "bmx" / "sampled-capture-fixtures")
        shutil.rmtree(checked)

    def test_vfr_encode_decode(self):
        result = module.finalize(self.workspace, self.manifest_path)
        self.assertEqual(result["decoded_frame_count"], 3)
        self.assertTrue(result["variable_pts_verified"])
        self.assertAlmostEqual(result["measured_sample_span_seconds"], 3.1)
        self.assertEqual(result["maximum_sample_gap_ms"], 1900)
        self.assertTrue(result["sampled_live"])
        self.assertFalse(result["full_motion_60fps"])
        self.assertFalse(result["visual_acceptance"])
        with self.assertRaisesRegex(ValueError, "already exists"):
            module.finalize(self.workspace, self.manifest_path)

    def test_hash_failure(self):
        self.manifest["samples"][0]["sha256"] = "0" * 64
        self.save()
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            module.finalize(self.workspace, self.manifest_path)

    def test_genuine_jpeg_source_encode_decode(self):
        for sample in self.manifest["samples"]:
            source = self.directory / sample["file"]
            target = source.with_suffix(".jpg")
            with Image.open(source) as image:
                image.save(target, format="JPEG", quality=90)
            sample["file"] = target.name
            sample["sha256"] = module.sha256(target)
        self.save()
        result = module.finalize(self.workspace, self.manifest_path)
        self.assertEqual(result["decoded_frame_count"], 3)
        self.assertTrue(result["variable_pts_verified"])

    def test_window_change(self):
        self.manifest["samples"][1]["window_id"] = 999
        self.save()
        with self.assertRaisesRegex(ValueError, "identity"):
            module.finalize(self.workspace, self.manifest_path)

    def test_time_reversal(self):
        self.manifest["samples"][1]["presentation_monotonic_ms"] = -1
        self.save()
        with self.assertRaisesRegex(ValueError, "interval"):
            module.finalize(self.workspace, self.manifest_path)

    def test_incomplete(self):
        self.manifest["completed"] = False
        self.save()
        with self.assertRaisesRegex(ValueError, "completed"):
            module.finalize(self.workspace, self.manifest_path)


if __name__ == "__main__":
    unittest.main()
