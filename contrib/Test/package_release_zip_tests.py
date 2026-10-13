import importlib.util
from pathlib import Path
import tempfile
import unittest
import zipfile

script = Path(__file__).resolve().parents[1] / "package_release_zip.py"
spec = importlib.util.spec_from_file_location("package_release_zip", script)
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseZipTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="release paths ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "staged release"
        (self.source / "licenses").mkdir(parents=True)
        (self.source / "licenses/imgui-LICENSE.txt").write_bytes(b"license contents\n")
        (self.source / "ReSkate.dll").write_bytes(b"fixture binary")
        self.output = self.root / "release.zip"

    def test_nested_files_extract_to_directories_with_contents_preserved(self):
        self.assertEqual(release.package(self.source, self.output), 2)
        release.check_archive(self.output)
        with zipfile.ZipFile(self.output) as archive:
            self.assertIn("licenses/imgui-LICENSE.txt", archive.namelist())
            self.assertFalse(any("\\" in name for name in archive.namelist()))
            archive.extractall(self.root / "extracted")
        self.assertEqual((self.root / "extracted/licenses/imgui-LICENSE.txt").read_bytes(), b"license contents\n")
        self.assertEqual((self.root / "extracted/ReSkate.dll").read_bytes(), b"fixture binary")

    def test_backslash_archive_is_rejected_without_changes(self):
        entry = zipfile.ZipInfo("licenses/imgui-LICENSE.txt")
        # Force the bad stored name; constructing ZipInfo normalizes it on Windows.
        entry.filename = "licenses\\imgui-LICENSE.txt"
        entry.orig_filename = entry.filename
        with zipfile.ZipFile(self.output, "w") as archive:
            archive.writestr(entry, b"license contents\n")
        with zipfile.ZipFile(self.output) as archive:
            self.assertIn("\\", archive.infolist()[0].orig_filename)
        before = self.output.read_bytes()
        with self.assertRaises(ValueError):
            release.check_archive(self.output)
        self.assertEqual(self.output.read_bytes(), before)

    def test_unsafe_and_duplicate_archive_names_are_rejected(self):
        for names in [("../escape.txt",), ("/absolute.txt",), ("C:/drive.txt",),
                      ("licenses//file.txt",), ("file.txt", "FILE.TXT")]:
            with self.subTest(names=names):
                with zipfile.ZipFile(self.output, "w") as archive:
                    for name in names:
                        archive.writestr(name, b"fixture")
                with self.assertRaises(ValueError):
                    release.check_archive(self.output)

    def test_symlink_archive_and_staging_are_refused(self):
        entry = zipfile.ZipInfo("linked.txt")
        entry.create_system = 3
        entry.external_attr = 0o120777 << 16
        with zipfile.ZipFile(self.output, "w") as archive:
            archive.writestr(entry, "outside.txt")
        with self.assertRaises(ValueError):
            release.check_archive(self.output)
        self.output.unlink()
        try:
            (self.source / "link").symlink_to(self.root / "missing")
        except OSError:
            self.skipTest("Creating symlinks is unavailable on this host")
        with self.assertRaises(ValueError):
            release.package(self.source, self.output)
        self.assertFalse(self.output.exists())

    def test_existing_output_is_preserved(self):
        self.output.write_bytes(b"existing release")
        with self.assertRaises(FileExistsError):
            release.package(self.source, self.output)
        self.assertEqual(self.output.read_bytes(), b"existing release")

    def test_output_inside_staging_is_refused(self):
        with self.assertRaises(ValueError):
            release.package(self.source, self.source / "release.zip")
        self.assertFalse((self.source / "release.zip").exists())

    def test_malformed_unix_staging_name_is_refused(self):
        if "\\" == __import__("os").sep:
            self.skipTest("Literal backslash filenames require a Unix filesystem")
        (self.source / "licenses\\lz4-LICENSE.txt").write_bytes(b"malformed name")
        with self.assertRaises(ValueError):
            release.package(self.source, self.output)
        self.assertFalse(self.output.exists())


if __name__ == "__main__":
    unittest.main()
