"""Known cases for the one-time switch tag fix of older regular recordings."""
import json
import os
import tempfile
import unittest
from datetime import datetime
from pathlib import Path

from fix_regular_skater_tags import REPORT_NAME, fix_regular_skater_tags

CUTOFF = datetime(2026, 10, 21)
OLDER = datetime(2026, 10, 19).timestamp()
NEWER = datetime(2026, 10, 22).timestamp()


def write_animation(path, switch, stance="regular", kind="render_capture", modified=OLDER):
    animation = {"format": "fourth-animation-studio", "version": 1, "name": path.stem, "stance": stance,
                 "source": {"kind": kind}, "tracks": []}
    if switch is not None:
        animation["switch_stance"] = switch
    path.write_text(json.dumps(animation, indent=2))
    os.utime(path, (modified, modified))
    return path


def switch_of(path):
    return json.loads(path.read_text()).get("switch_stance")


class FixRegularSkaterTagsTests(unittest.TestCase):
    def setUp(self):
        self.root_holder = tempfile.TemporaryDirectory()
        root = Path(self.root_holder.name)
        self.tricks = root / "ReSkate.Tricks"
        self.output = root / "ReSkate.Tricks-fixed"
        (self.tricks / "recorded").mkdir(parents=True)

    def tearDown(self):
        self.root_holder.cleanup()

    def fixed(self, path):
        return self.output / path.relative_to(self.tricks)

    def test_regular_recordings_flip_in_the_copy_only(self):
        take = write_animation(self.tricks / "recorded" / "heelflip__mid.json", True)
        edited = write_animation(self.tricks / "my_heelflip.json", False)
        changed = fix_regular_skater_tags(self.tricks, self.output, CUTOFF)
        self.assertEqual(sorted(changed), sorted([Path("recorded/heelflip__mid.json"), Path("my_heelflip.json")]))
        self.assertIs(switch_of(self.fixed(take)), False)
        self.assertIs(switch_of(self.fixed(edited)), True)
        self.assertIs(switch_of(take), True)
        self.assertIs(switch_of(edited), False)

    def test_recordings_in_their_own_folder_are_fixed_too(self):
        # The studio saves a trick as its own folder (Name/Name.json) now. Those are recordings like any
        # other and were being passed over.
        (self.tricks / "Kickflip 2").mkdir()
        own = write_animation(self.tricks / "Kickflip 2" / "Kickflip 2.json", True)
        (self.tricks / "presets").mkdir()
        (self.tricks / "presets" / "current.txt").write_text("Steezy")
        changed = fix_regular_skater_tags(self.tricks, self.output, CUTOFF)
        self.assertIn(Path("Kickflip 2/Kickflip 2.json"), changed)
        self.assertIs(switch_of(self.fixed(own)), False)
        self.assertIs(switch_of(own), True)

    def test_a_recording_without_a_stance_is_regular(self):
        take = write_animation(self.tricks / "recorded" / "ollie__mid.json", True, stance="")
        fix_regular_skater_tags(self.tricks, self.output, CUTOFF)
        self.assertIs(switch_of(self.fixed(take)), False)

    def test_goofy_game_clip_untagged_and_newer_files_stay(self):
        goofy = write_animation(self.tricks / "goofy.json", True, stance="goofy")
        game_clip = write_animation(self.tricks / "game_clip.json", False, kind="native_asset")
        untagged = write_animation(self.tricks / "untagged.json", None)
        newer = write_animation(self.tricks / "saved_by_new_build.json", True, modified=NEWER)
        self.assertEqual(fix_regular_skater_tags(self.tricks, self.output, CUTOFF), [])
        for path in (goofy, game_clip, untagged, newer):
            self.assertEqual(self.fixed(path).read_bytes(), path.read_bytes())

    def test_only_the_tag_changes_and_other_files_are_copied(self):
        take = write_animation(self.tricks / "recorded" / "kickflip__mid.json", True)
        clip = self.tricks / "recorded" / "kickflip__mid.clip"
        clip.write_bytes(b"FAS3 clip bytes")
        fix_regular_skater_tags(self.tricks, self.output, CUTOFF)
        self.assertEqual(self.fixed(take).read_text(),
                         take.read_text().replace('"switch_stance": true', '"switch_stance": false'))
        self.assertEqual(self.fixed(clip).read_bytes(), clip.read_bytes())

    def test_the_report_lists_the_changed_files(self):
        write_animation(self.tricks / "recorded" / "kickflip__mid.json", True)
        fix_regular_skater_tags(self.tricks, self.output, CUTOFF)
        self.assertIn("recorded/kickflip__mid.json", (self.output / REPORT_NAME).read_text())

    def test_an_existing_output_folder_is_refused(self):
        self.output.mkdir()
        with self.assertRaises(ValueError):
            fix_regular_skater_tags(self.tricks, self.output, CUTOFF)

    def test_a_folder_fixed_before_is_refused(self):
        write_animation(self.tricks / "recorded" / "kickflip__mid.json", True)
        fix_regular_skater_tags(self.tricks, self.output, CUTOFF)
        with self.assertRaises(ValueError):
            fix_regular_skater_tags(self.output, self.output.with_name("again"), CUTOFF)


if __name__ == "__main__":
    unittest.main()
