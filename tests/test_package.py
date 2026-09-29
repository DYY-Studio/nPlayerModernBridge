import os
import shutil
import stat
import tempfile
import unittest
import warnings
from unittest import mock
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile, ZipInfo

from npabridge import package
from npabridge.macho import parse
from npabridge.manifest import load_manifest

from support import SOURCE_IPA

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "package"
MAIN = ROOT / "build" / "macho" / "main-phase-b"
BRIDGE = ROOT / "build" / "LibASSBridge.dylib"
MANIFEST = load_manifest(ROOT / "manifests" / "nplayer-3.13.0.json")
BASENAME = MANIFEST.dylib("libass").basename
BRIDGES = {BASENAME: BRIDGE}


class _PackageFixture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.source = self.root / "source.ipa"
        self.output = self.root / "output.ipa"
        self.main = self.root / "signed-main"
        self.main.write_bytes(b"signed-main")
        self.bridge_name = "ExampleBridge.dylib"
        self.bridge = self.root / self.bridge_name
        self.bridge.write_bytes(b"signed-bridge")
        self._write_source()

    def tearDown(self):
        self.temporary.cleanup()

    def _write_source(self):
        main = ZipInfo(package.MAIN_MEMBER, (2024, 1, 2, 3, 4, 6))
        main.compress_type = ZIP_STORED
        main.comment = b"main metadata"
        main.extra = b"\xfe\xca\x00\x00"
        main.create_system = 3
        main.create_version = 45
        main.extract_version = 20
        main.internal_attr = 1
        main.external_attr = (stat.S_IFREG | 0o755) << 16

        unrelated = ZipInfo("Payload/nPlayer.app/Info.plist", (2023, 2, 4, 6, 8, 10))
        unrelated.compress_type = ZIP_DEFLATED
        unrelated.comment = b"unrelated metadata"
        unrelated.extra = b"\xef\xbe\x00\x00"
        unrelated.create_system = 3
        unrelated.create_version = 30
        unrelated.extract_version = 20
        unrelated.internal_attr = 1
        unrelated.external_attr = (stat.S_IFREG | 0o644) << 16

        frameworks = ZipInfo(f"{package.FRAMEWORKS.as_posix()}/")
        frameworks.create_system = 3
        frameworks.external_attr = (stat.S_IFDIR | 0o755) << 16

        with ZipFile(self.source, "w") as archive:
            archive.comment = b"source archive comment"
            archive.writestr(main, b"old-main")
            archive.writestr(unrelated, b"plist-data")
            archive.writestr(frameworks, b"")


class ArchiveWriterTests(_PackageFixture):
    def test_preserves_source_entries_and_replaces_the_main(self):
        with ZipFile(self.source) as archive:
            source_infos = {info.filename: info for info in archive.infolist()}

        report = package._write_ipa(
            self.source,
            self.output,
            self.main,
            {self.bridge_name: self.bridge},
        )

        with ZipFile(self.output) as archive:
            infos = archive.infolist()
            output_infos = {info.filename: info for info in infos}
            self.assertEqual(archive.comment, b"source archive comment")
            self.assertEqual(archive.read(package.MAIN_MEMBER), b"signed-main")
            self.assertEqual(
                archive.read("Payload/nPlayer.app/Info.plist"), b"plist-data"
            )
            self.assertEqual(
                archive.read(package.bridge_member(self.bridge_name)),
                b"signed-bridge",
            )

        self.assertEqual(
            [info.filename for info in infos],
            [
                package.MAIN_MEMBER,
                "Payload/nPlayer.app/Info.plist",
                f"{package.FRAMEWORKS.as_posix()}/",
                package.bridge_member(self.bridge_name),
            ],
        )
        fields = (
            "date_time",
            "compress_type",
            "comment",
            "extra",
            "create_system",
            "create_version",
            "extract_version",
            "internal_attr",
            "external_attr",
        )
        for member in source_infos:
            with self.subTest(member=member):
                self.assertEqual(
                    tuple(getattr(output_infos[member], field) for field in fields),
                    tuple(getattr(source_infos[member], field) for field in fields),
                )
        bridge_mode = output_infos[package.bridge_member(self.bridge_name)].external_attr >> 16
        self.assertEqual(stat.S_IFMT(bridge_mode), stat.S_IFREG)
        self.assertEqual(stat.S_IMODE(bridge_mode), 0o755)
        self.assertEqual(
            report,
            {"main_members": 1, "bridge_members": {self.bridge_name: 1}},
        )

    def test_rejects_a_missing_main(self):
        with ZipFile(self.source, "w") as archive:
            archive.writestr("Payload/nPlayer.app/Info.plist", b"plist")
        with self.assertRaisesRegex(ValueError, "0 main executables"):
            package._write_ipa(
                self.source, self.output, self.main, {self.bridge_name: self.bridge}
            )
        self.assertFalse(self.output.exists())


class PackageOrchestrationTests(_PackageFixture):
    def test_windows_prefers_the_root_ldid_over_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            root_ldid = root / "ldid.exe"
            root_ldid.write_bytes(b"ldid")
            with (
                mock.patch.object(package, "ROOT", root),
                mock.patch("sys.platform", "win32"),
                mock.patch.object(
                    package.shutil,
                    "which",
                    side_effect=AssertionError("searched PATH"),
                ),
            ):
                self.assertEqual(package._tool("ldid"), str(root_ldid.resolve()))

    def test_missing_ldid_error_includes_the_windows_release(self):
        with tempfile.TemporaryDirectory() as directory:
            with (
                mock.patch.object(package, "ROOT", Path(directory)),
                mock.patch.object(package.shutil, "which", return_value=None),
            ):
                with self.assertRaisesRegex(
                    RuntimeError, r"Windows.*ldid_w64_x86_64"
                ):
                    package._tool("ldid")

    def test_signs_scratch_copies_without_external_archive_tools(self):
        output = self.root / "output directory" / "patched app.ipa"
        work = self.root / "work directory"
        original_main = self.main.read_bytes()
        original_bridge = self.bridge.read_bytes()
        signed = []

        def sign_copy(path):
            signed.append(path)
            path.write_bytes(path.read_bytes() + b"-signed")

        with (
            mock.patch.object(package, "sign", side_effect=sign_copy),
            mock.patch.object(
                package,
                "_tool",
                side_effect=AssertionError("resolved an external archive tool"),
            ),
        ):
            report = package.package_ipa(
                self.source,
                output,
                self.main,
                {self.bridge_name: self.bridge},
                work=work,
            )

        self.assertEqual(len(signed), 2)
        self.assertTrue(all(path != self.main and path != self.bridge for path in signed))
        self.assertTrue(all(work in path.parents for path in signed))
        self.assertEqual(self.main.read_bytes(), original_main)
        self.assertEqual(self.bridge.read_bytes(), original_bridge)
        with ZipFile(output) as archive:
            self.assertEqual(archive.read(package.MAIN_MEMBER), b"signed-main-signed")
            self.assertEqual(
                archive.read(package.bridge_member(self.bridge_name)),
                b"signed-bridge-signed",
            )
        self.assertEqual(report["main_members"], 1)
        self.assertEqual(report["bridge_members"], {self.bridge_name: 1})

    def test_default_scratch_directory_is_cleaned_after_success(self):
        with tempfile.TemporaryDirectory() as isolated:
            with (
                mock.patch.object(tempfile, "tempdir", isolated),
                mock.patch.object(package, "sign"),
                mock.patch.object(
                    package,
                    "_tool",
                    side_effect=AssertionError("resolved an external archive tool"),
                ),
            ):
                package.package_ipa(
                    self.source,
                    self.output,
                    self.main,
                    {self.bridge_name: self.bridge},
                )
            self.assertEqual(os.listdir(isolated), [])

    def test_signing_failure_cleans_scratch_and_does_not_publish(self):
        with tempfile.TemporaryDirectory() as isolated:
            with (
                mock.patch.object(tempfile, "tempdir", isolated),
                mock.patch.object(
                    package, "sign", side_effect=RuntimeError("signing failed")
                ),
            ):
                with self.assertRaisesRegex(RuntimeError, "signing failed"):
                    package.package_ipa(
                        self.source,
                        self.output,
                        self.main,
                        {self.bridge_name: self.bridge},
                    )
            self.assertEqual(os.listdir(isolated), [])
            self.assertFalse(self.output.exists())

    def test_archive_failure_removes_partial_output_and_scratch(self):
        temporary_output = self.output.with_name(f".tmp-{self.output.name}")

        def fail_write(source, output, main, bridges):
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(b"partial")
            raise OSError("archive write failed")

        with tempfile.TemporaryDirectory() as isolated:
            with (
                mock.patch.object(tempfile, "tempdir", isolated),
                mock.patch.object(package, "sign"),
                mock.patch.object(package, "_write_ipa", side_effect=fail_write),
            ):
                with self.assertRaisesRegex(OSError, "archive write failed"):
                    package.package_ipa(
                        self.source,
                        self.output,
                        self.main,
                        {self.bridge_name: self.bridge},
                    )
            self.assertEqual(os.listdir(isolated), [])
            self.assertFalse(temporary_output.exists())
            self.assertFalse(self.output.exists())

    def test_rejects_a_duplicate_main(self):
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with ZipFile(self.source, "a") as archive:
                archive.writestr(package.MAIN_MEMBER, b"duplicate")
        with self.assertRaisesRegex(ValueError, "2 main executables"):
            package._write_ipa(
                self.source, self.output, self.main, {self.bridge_name: self.bridge}
            )
        self.assertFalse(self.output.exists())

    def test_rejects_an_existing_selected_bridge(self):
        with ZipFile(self.source, "a") as archive:
            archive.writestr(package.bridge_member(self.bridge_name), b"old bridge")
        with self.assertRaisesRegex(ValueError, "already carries"):
            package._write_ipa(
                self.source, self.output, self.main, {self.bridge_name: self.bridge}
            )
        self.assertFalse(self.output.exists())

    def test_rejects_frameworks_when_it_is_a_file(self):
        with ZipFile(self.source, "w") as archive:
            archive.writestr(package.MAIN_MEMBER, b"old-main")
            archive.writestr(package.FRAMEWORKS.as_posix(), b"not a directory")
        with self.assertRaisesRegex(ValueError, "Frameworks.*not a directory"):
            package._write_ipa(
                self.source, self.output, self.main, {self.bridge_name: self.bridge}
            )
        self.assertFalse(self.output.exists())

    def test_removes_partial_output_after_a_write_failure(self):
        real_writestr = ZipFile.writestr
        calls = 0

        def fail_second_write(archive, *args, **kwargs):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError("injected archive failure")
            return real_writestr(archive, *args, **kwargs)

        with mock.patch.object(ZipFile, "writestr", fail_second_write):
            with self.assertRaisesRegex(OSError, "injected archive failure"):
                package._write_ipa(
                    self.source,
                    self.output,
                    self.main,
                    {self.bridge_name: self.bridge},
                )
        self.assertFalse(self.output.exists())


class PackageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SOURCE_IPA.is_file():
            raise unittest.SkipTest("source IPA is not present")
        if not MAIN.is_file() or not BRIDGE.is_file():
            raise unittest.SkipTest("patch artifacts are not built")
        cls.artifact = BUILD / "test" / "patched.ipa"
        package.publish(SOURCE_IPA, cls.artifact, MAIN, BRIDGES)

    def test_frameworks_path_that_is_a_file_is_rejected(self):
        crafted = BUILD / "test" / "frameworks-file.ipa"
        crafted.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(SOURCE_IPA, crafted)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with ZipFile(crafted, "a") as archive:
                archive.writestr(f"{package.APP_DIR}/Frameworks", b"")
        output = BUILD / "test" / "frameworks-file-out.ipa"
        output.unlink(missing_ok=True)
        with self.assertRaises(ValueError) as caught:
            package.publish(crafted, output, MAIN, BRIDGES)
        self.assertIn("Frameworks", str(caught.exception))
        self.assertFalse(output.exists())

    def test_default_scratch_directory_is_cleaned_up(self):
        with tempfile.TemporaryDirectory() as isolated:
            with mock.patch.object(tempfile, "tempdir", isolated):
                output = BUILD / "test" / "scratch.ipa"
                package.publish(SOURCE_IPA, output, MAIN, BRIDGES)
                self.assertEqual(os.listdir(isolated), [])
                self.assertTrue(output.is_file())

    def test_artifact_carries_one_main_and_every_selected_bridge(self):
        report = package.inspect_ipa(self.artifact, (BASENAME,))
        self.assertEqual(
            report, {"main_members": 1, "bridge_members": {BASENAME: 1}}
        )
        with ZipFile(self.artifact) as archive:
            names = archive.namelist()
            self.assertIn(package.MAIN_MEMBER, names)
            self.assertIn(package.bridge_member(BASENAME), names)
            main = archive.read(package.MAIN_MEMBER)
            bridge = archive.read(package.bridge_member(BASENAME))
        for name, content in (("main", main), ("bridge", bridge)):
            target = BUILD / "test" / f"extracted-{name}"
            target.write_bytes(content)
            parsed = parse(target)
            with self.subTest(artifact=name):
                self.assertTrue(parsed.has_code_signature)
                self.assertEqual(parsed.build_version.minos[:2], [13, 0])


if __name__ == "__main__":
    unittest.main()
