import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("mac_build", Path(__file__).parents[1] / "build.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


class ArchiveTests(unittest.TestCase):
    def test_corrupt_cached_archive_is_refused_before_extraction(self):
        with tempfile.TemporaryDirectory() as folder:
            cache = Path(folder)
            name = next(iter(build.ARCHIVES))
            (cache / name).write_bytes(b"a truncated download")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                build.archive(cache, name)

    def test_copy_keeps_framework_symlinks(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "source with spaces"
            source.mkdir()
            (source / "Versions").mkdir()
            (source / "Versions/A").write_bytes(b"framework")
            (source / "Current").symlink_to("Versions/A")
            build.copytree(source, root / "output")
            self.assertTrue((root / "output/Current").is_symlink())
            self.assertEqual((root / "output/Current").read_bytes(), b"framework")


if __name__ == "__main__":
    unittest.main()
