import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import stage_studio_tester as tester


class TesterPackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(); self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name); self.build = self.root / 'build'; self.repo = self.root / 'repo'
        (self.build / 'Release').mkdir(parents=True); (self.repo / 'External/imgui').mkdir(parents=True)
        (self.repo / 'External/imgui/LICENSE.txt').write_text('fixture license')
        (self.build / 'CMakeCache.txt').write_text('DINGOSDK_LAUNCHER_AUTO_UPDATE:BOOL=OFF\n')
        for name in ('ReSkate.dll', 'ReSkateLauncher.exe'): (self.build / 'Release' / name).write_bytes(b'MZ fixture')
        self.output = self.root / 'stage'; self.archive = self.root / 'library.zip'

    def make_archive(self, codec='native-piec-v1-extent', extra=None):
        files = {'ReSkate.Tricks/library/catalog.json': json.dumps([dict(file='flip.clip', position_codec=codec)]).encode(),
                 'ReSkate.Tricks/library/native-catalog.json': b'[{},{}]', 'ReSkate.Tricks/library/flip.clip': b'fixture clip'}
        manifest = dict(files=[dict(path=n, size=len(data), sha256=hashlib.sha256(data).hexdigest()) for n, data in files.items()])
        with zipfile.ZipFile(self.archive, 'w') as archive:
            for name, data in files.items(): archive.writestr(name, data)
            archive.writestr('manifest.json', json.dumps(manifest))
            archive.writestr('READ-ME-FIRST.txt', 'source readme')
            if extra: archive.writestr(*extra)
        return dict(sha256=tester.sha256(self.archive), bytes=self.archive.stat().st_size, previews=1,
                    inventory=2, unsupported=1, steam_build='fixture')

    def test_complete_package_preserves_payloads_and_identifies_candidate(self):
        info = tester.stage(self.repo, self.build, self.archive_for_success(), self.output, self.config, 'fixture-commit')
        self.assertEqual((self.output / 'ReSkate.Tricks/library/flip.clip').read_bytes(), b'fixture clip')
        self.assertEqual((self.output / 'licenses/imgui/LICENSE.txt').read_text(), 'fixture license')
        self.assertEqual(info['SourceCommit'], 'fixture-commit'); self.assertFalse(info['BinaryAutoUpdate'])
        self.assertIn('tester candidate', (self.output / 'READ-ME-FIRST.txt').read_text())
        self.assertIn('--no-update', (self.output / 'START-STUDIO.cmd').read_text())
        self.assertFalse((self.output / 'ReSkate.Tricks/presets').exists())

    def archive_for_success(self):
        self.config = self.make_archive(); return self.archive

    def test_tampered_library_fails_before_staging(self):
        config = self.make_archive(); self.archive.write_bytes(b'changed')
        with self.assertRaises(ValueError): tester.stage(self.repo, self.build, self.archive, self.output, config, 'commit')
        self.assertFalse(self.output.exists())

    def test_outdated_catalog_cannot_ship(self):
        config = self.make_archive(codec='legacy')
        with self.assertRaises(ValueError): tester.stage(self.repo, self.build, self.archive, self.output, config, 'commit')
        self.assertFalse(self.output.exists())

    def test_library_cannot_include_personal_files_or_escape_paths(self):
        for name in ('ReSkate.Tricks/presets/personal.json', '../escape.txt'):
            with self.subTest(name=name):
                config = self.make_archive(extra=(name, 'not allowed'))
                with self.assertRaises(ValueError): tester.stage(self.repo, self.build, self.archive, self.output, config, 'commit')
                self.assertFalse(self.output.exists())

    def test_missing_binary_and_auto_update_are_rejected(self):
        config = self.make_archive()
        (self.build / 'CMakeCache.txt').write_text('DINGOSDK_LAUNCHER_AUTO_UPDATE:BOOL=ON')
        with self.assertRaises(ValueError): tester.stage(self.repo, self.build, self.archive, self.output, config, 'commit')
        (self.build / 'CMakeCache.txt').write_text('DINGOSDK_LAUNCHER_AUTO_UPDATE:BOOL=OFF')
        (self.build / 'Release/ReSkate.dll').unlink()
        with self.assertRaises(FileNotFoundError): tester.stage(self.repo, self.build, self.archive, self.output, config, 'commit')
        self.assertFalse(self.output.exists())

    def test_existing_stage_remains_unchanged(self):
        config = self.make_archive(); self.output.mkdir(); (self.output / 'keep').write_text('original')
        with self.assertRaises(FileExistsError): tester.stage(self.repo, self.build, self.archive, self.output, config, 'commit')
        self.assertEqual((self.output / 'keep').read_text(), 'original')


if __name__ == '__main__': unittest.main()
