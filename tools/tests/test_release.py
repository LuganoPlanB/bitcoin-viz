import importlib.util
import json
import subprocess
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("bwc_release", Path(__file__).resolve().parents[1] / "release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTools(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="bwc-release-")
        self.root = Path(self.temp.name)
        (self.root / "src/bitcoin_wizard_cave").mkdir(parents=True)
        (self.root / "src/bitcoin_wizard_cave/__init__.py").write_text('__version__ = "0.2.0"\n')
        (self.root / "pyproject.toml").write_text('[project]\nname = "bitcoin-wizard-cave"\nversion = "0.2.0"\n')
        (self.root / "VERSION").write_text("0.2.0\n")
        (self.root / "README.md").write_text("Bitcoin Wizard Cave\n")
        (self.root / ".gitignore").write_text(".release/\n")
        (self.root / "vcpkg.json").write_text(json.dumps({"builtin-baseline": "1" * 40}))
        subprocess.run(["git", "init", "-q"], cwd=self.root, check=True)
        release.git(self.root, "add", ".")
        release.git(self.root, "-c", "user.name=Test", "-c", "user.email=test@example.invalid", "commit", "-qm", "baseline")
        self.base = release.git(self.root, "rev-parse", "HEAD")

    def tearDown(self): self.temp.cleanup()

    def stage(self, platform, arch):
        stage = self.root / ".release" / f"stage-{platform}-{arch}"
        (stage / "bin").mkdir(parents=True)
        binary = "bitcoin-wizard-cave-native" + (".exe" if platform == "windows" else "")
        (stage / "bin" / binary).write_bytes(b"fixture executable")
        licenses = stage / "share/bitcoin-wizard-cave"
        licenses.mkdir(parents=True)
        (licenses / "LICENSE").write_text("GPL\n")
        (licenses / "nlohmann-LICENSE.MIT").write_text("MIT\n")
        return stage

    def test_prepare_versions_exact_source_commit_and_bundle(self):
        bundle = self.root / ".release/release.bundle"
        commit = release.prepare(self.root, "0.3.0", self.base, bundle)
        self.assertNotEqual(commit, self.base)
        self.assertEqual(release.git(self.root, "show", f"{commit}:VERSION"), "0.3.0")
        self.assertIn('version = "0.3.0"', (self.root / "pyproject.toml").read_text())
        self.assertIn('__version__ = "0.3.0"', (self.root / "src/bitcoin_wizard_cave/__init__.py").read_text())
        self.assertEqual(release.git(self.root, "rev-parse", "HEAD^"), self.base)
        subprocess.run(["git", "bundle", "verify", str(bundle)], cwd=self.root, check=True, capture_output=True)

    def test_repeated_prepare_preserves_candidate_and_bundle(self):
        bundle = self.root / ".release/release.bundle"
        commit = release.prepare(self.root, "0.3.0", self.base, bundle)
        again = release.prepare(self.root, "0.3.0", commit, bundle)
        self.assertEqual(again, commit)

    def test_malformed_versions_are_rejected_without_writes(self):
        for value in ("v1.0.0", "1.2", "1.0.0\n", "01.2.3", "1.2.3;echo bad", "1.2.3-rc1"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                release.prepare(self.root, value, self.base, self.root / ".release/release.bundle")
        self.assertEqual((self.root / "VERSION").read_text(), "0.2.0\n")

    def test_unexpected_head_and_dirty_source_are_rejected(self):
        with self.assertRaises(ValueError): release.prepare(self.root, "1.0.0", "0" * 40, self.root / ".release/bundle")
        (self.root / "README.md").write_text("uncommitted edit")
        with self.assertRaises(ValueError): release.prepare(self.root, "1.0.0", self.base, self.root / ".release/bundle")

    def test_all_five_archives_have_matching_manifests_and_checksums(self):
        output = self.root / ".release/assets"
        for platform, arch in release.TARGETS:
            release.package(self.root, self.stage(platform, arch), output, "0.3.0", platform, arch)
        sums = release.collect(output, "0.3.0", self.base)
        self.assertEqual(len(sums.read_text().splitlines()), 5)
        self.assertIn("linux-arm64.tar.gz", sums.read_text())
        self.assertIn("windows-x86_64.zip", sums.read_text())

    def test_partial_release_and_wrong_commit_are_rejected(self):
        output = self.root / ".release/assets"
        release.package(self.root, self.stage("linux", "x86_64"), output, "0.3.0", "linux", "x86_64")
        with self.assertRaises(ValueError): release.collect(output, "0.3.0", self.base)
        for platform, arch in release.TARGETS - {("linux", "x86_64")}:
            release.package(self.root, self.stage(platform, arch), output, "0.3.0", platform, arch)
        with self.assertRaises(ValueError): release.collect(output, "0.3.0", "0" * 40)

    def test_missing_executable_and_licenses_are_rejected(self):
        stage = self.stage("windows", "x86_64")
        (stage / "bin/bitcoin-wizard-cave-native.exe").unlink()
        with self.assertRaises(ValueError): release.package(self.root, stage, self.root / ".release/assets", "0.3.0", "windows", "x86_64")
        stage = self.stage("linux", "arm64")
        (stage / "share/bitcoin-wizard-cave/LICENSE").unlink()
        with self.assertRaises(ValueError): release.package(self.root, stage, self.root / ".release/assets", "0.3.0", "linux", "arm64")

    def test_dependency_license_set_is_required(self):
        deps = self.root / ".release/deps"
        deps.mkdir(parents=True)
        stage = self.stage("linux", "x86_64")
        with self.assertRaises(ValueError): release.package(self.root, stage, self.root / ".release/assets", "0.3.0", "linux", "x86_64", deps)
        for name in ("curl", "openssl", "sqlite3", "zeromq"):
            target = deps / "share" / name
            target.mkdir(parents=True)
            (target / "copyright").write_text(name + " license")
        archive = release.package(self.root, stage, self.root / ".release/assets", "0.3.0", "linux", "x86_64", deps)
        self.assertTrue(archive.is_file())
        self.assertEqual(len(list((stage / "share/bitcoin-wizard-cave/third-party").glob("*.txt"))), 4)


if __name__ == "__main__": unittest.main()
