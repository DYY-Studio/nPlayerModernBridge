import hashlib
import shutil
import struct
import tempfile
import unittest
import warnings
from dataclasses import replace
from pathlib import Path
from zipfile import ZipFile

from npabridge import package, patch
from npabridge.manifest import Domain, Dylib, select_manifest
from npabridge.verify import VerificationError

from support import ROOT, SOURCE_IPA


MANIFESTS = ROOT / "manifests"
BUILD = ROOT / "build"
BRIDGE = BUILD / "LibASSBridge.dylib"
BASENAME = "LibASSBridge.dylib"
# the main member of the device-accepted bridge.ipa, signed under the name nPlayer
PACKAGED_MAIN_SHA256 = "e84ef5b5e10cb10940ecffe73c3509f932a4aa6d2cba053052a7d9e7549792fe"
# the default selection: libass plus the whole FFmpeg 4.4.8 in one dylib
FULL_PACKAGED_MAIN_SHA256 = (
    "f22d7af623272032e3c529b0cfc0e1b9340b42e310756f6b817f438e57f6758a"
)
# the split alternative: libass plus core 4.4.8 plus the 9.0.2 scaler/resampler,
# i.e. the three-unit artifact the bridge was device-accepted on
SPLIT_PACKAGED_MAIN_SHA256 = (
    "638c00d9602b2797d3f18030ebc6ada4bf825a3f871f2f549374fbdecddcdd78"
)


def _patched(source, output, work, **keywords):
    return patch.patch_ipa(source, output, BUILD, MANIFESTS, work=work, **keywords)


class PatchFlowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SOURCE_IPA.is_file():
            raise unittest.SkipTest("source IPA is not present")
        if not BRIDGE.is_file():
            raise unittest.SkipTest("LibASSBridge.dylib is not built")
        cls.work = BUILD / "patch" / "test"
        cls.work.mkdir(parents=True, exist_ok=True)

    def test_libass_only_patch_reproduces_the_accepted_artifact(self):
        expected = SOURCE_IPA.with_name(f"{SOURCE_IPA.stem}-libass0.17.5.ipa")
        expected.unlink(missing_ok=True)
        try:
            result = _patched(
                SOURCE_IPA, None, self.work / "run", dylibs=["libass"]
            )
            self.assertEqual(result.output, expected.resolve())
            self.assertEqual(result.app_version, "3.13.0")
            self.assertEqual(result.dylibs, ("libass",))
            self.assertEqual(result.state_initial, 0)
            self.assertGreater(result.checks_passed, 0)
            with ZipFile(result.output) as archive:
                main = archive.read(patch.MAIN_MEMBER)
            self.assertEqual(hashlib.sha256(main).hexdigest(), PACKAGED_MAIN_SHA256)
        finally:
            expected.unlink(missing_ok=True)

    def test_default_work_directory_is_reported_and_cleaned_up(self):
        output = self.work / "temp-work.ipa"
        output.unlink(missing_ok=True)
        temp_root = Path(tempfile.gettempdir())
        before = set(temp_root.glob("npa-patch-*"))
        result = patch.patch_ipa(
            SOURCE_IPA, output, BUILD, MANIFESTS, dylibs=["libass"]
        )
        self.assertEqual(set(temp_root.glob("npa-patch-*")), before)
        self.assertEqual(result.packaged_main_sha256, PACKAGED_MAIN_SHA256)
        self.assertEqual(
            set(result.bridge_sha256s), {"LibASSBridge.dylib"}
        )
        self.assertTrue(output.is_file())

    def test_default_run_installs_the_default_selection(self):
        expected = SOURCE_IPA.with_name(
            f"{SOURCE_IPA.stem}-libass0.17.5-ffmpeg-full4.4.8.ipa"
        )
        expected.unlink(missing_ok=True)
        try:
            result = _patched(SOURCE_IPA, None, self.work / "both")
            self.assertEqual(result.output, expected.resolve())
            self.assertEqual(result.dylibs, ("libass", "ffmpeg-full"))
            self.assertEqual(result.state_initial, 0)
            self.assertEqual(
                set(result.bridge_sha256s),
                {
                    "LibASSBridge.dylib",
                    "LibFFmpegFullBridge.dylib",
                },
            )
            with ZipFile(result.output) as archive:
                names = archive.namelist()
            for basename in (
                "LibASSBridge.dylib",
                "LibFFmpegFullBridge.dylib",
            ):
                self.assertEqual(
                    names.count(f"{package.APP_DIR}/Frameworks/{basename}"), 1
                )
            self.assertEqual(
                result.packaged_main_sha256, FULL_PACKAGED_MAIN_SHA256
            )
        finally:
            expected.unlink(missing_ok=True)

    def test_split_selection_matches_the_device_accepted_anchor(self):
        output = self.work / "split.ipa"
        output.unlink(missing_ok=True)
        try:
            result = _patched(
                SOURCE_IPA,
                output,
                self.work / "split",
                dylibs=["libass", "ffmpeg", "ffmpeg-core"],
            )
            self.assertEqual(result.dylibs, ("libass", "ffmpeg", "ffmpeg-core"))
            self.assertEqual(
                result.packaged_main_sha256, SPLIT_PACKAGED_MAIN_SHA256
            )
        finally:
            output.unlink(missing_ok=True)

    def test_conflicting_dylibs_are_rejected(self):
        output = self.work / "conflict.ipa"
        output.unlink(missing_ok=True)
        with self.assertRaises(ValueError) as caught:
            _patched(
                SOURCE_IPA,
                output,
                self.work / "conflict",
                dylibs=["ffmpeg-full", "ffmpeg-core"],
            )
        self.assertIn("conflicting dylib selection", str(caught.exception))
        self.assertFalse(output.exists())

    def test_unknown_dylib_id_is_rejected(self):
        with self.assertRaises(KeyError) as caught:
            _patched(
                SOURCE_IPA,
                self.work / "unknown.ipa",
                self.work / "unknown",
                dylibs=["libavcodec"],
            )
        self.assertIn("libavcodec", str(caught.exception))

    def test_default_output_name_follows_the_manifest_order(self):
        manifest = _manifest_with_an_extra_dylib()
        self.assertEqual(
            patch.default_output_name(SOURCE_IPA, manifest, manifest.units()).name,
            "nPlayer_3.13.0-libass0.17.5-ffmpeg-full4.4.8-other1.0.0.ipa",
        )
        self.assertEqual(
            patch.default_output_name(
                SOURCE_IPA, manifest, manifest.units(("ffmpeg",))
            ).name,
            "nPlayer_3.13.0-ffmpeg9.0.2.ipa",
        )
        self.assertEqual(
            patch.default_output_name(
                SOURCE_IPA, manifest, manifest.units(("other",))
            ).name,
            "nPlayer_3.13.0-other1.0.0.ipa",
        )

    def test_refuses_to_overwrite_the_bridge_dylib(self):
        directory = self.work / "overwrite-bridge"
        directory.mkdir(parents=True, exist_ok=True)
        dylib = directory / BASENAME
        shutil.copy2(BRIDGE, dylib)
        before = hashlib.sha256(dylib.read_bytes()).hexdigest()
        with self.assertRaises(ValueError) as caught:
            patch.patch_ipa(
                SOURCE_IPA,
                dylib,
                directory,
                MANIFESTS,
                dylibs=["libass"],
                work=self.work / "overwrite",
            )
        self.assertIn("bridge", str(caught.exception).lower())
        self.assertEqual(hashlib.sha256(dylib.read_bytes()).hexdigest(), before)

    def test_missing_bridge_dylib_is_reported(self):
        missing = self.work / "no-dylibs-here"
        missing.mkdir(parents=True, exist_ok=True)
        with self.assertRaises(FileNotFoundError) as caught:
            patch.patch_ipa(
                SOURCE_IPA,
                self.work / "missing.ipa",
                missing,
                MANIFESTS,
                dylibs=["libass"],
                work=self.work / "missing-work",
            )
        self.assertIn(BASENAME, str(caught.exception))

    def test_unsupported_version_lists_the_supported_one(self):
        unknown = self.work / "unknown-main"
        unknown.write_bytes(b"\x00" * 16)
        with self.assertRaises(ValueError) as caught:
            select_manifest(MANIFESTS, unknown)
        message = str(caught.exception)
        self.assertIn("3.13.0", message)
        self.assertIn(hashlib.sha256(b"\x00" * 16).hexdigest(), message)

    def test_encrypted_input_is_reported_as_encrypted(self):
        encrypted = self.work / "encrypted.ipa"
        self._write_ipa_with_crypt_id(SOURCE_IPA, encrypted, 1)
        output = self.work / "encrypted-out.ipa"
        with self.assertRaises(ValueError) as caught:
            _patched(encrypted, output, self.work / "encrypted-work")
        self.assertIn("encrypted", str(caught.exception).lower())
        self.assertFalse(output.exists())

    def test_invalid_bridge_is_rejected_by_name(self):
        directory = self.work / "bad-bridge-dir"
        extracted = package.extract_for_verification(
            SOURCE_IPA, self.work / "bad-bridge", (BASENAME,)
        )
        directory.mkdir(parents=True, exist_ok=True)
        shutil.copy2(extracted["main"], directory / BASENAME)
        output = self.work / "bad-bridge.ipa"
        with self.assertRaises(VerificationError) as caught:
            patch.patch_ipa(
                SOURCE_IPA,
                output,
                directory,
                MANIFESTS,
                dylibs=["libass"],
                work=self.work / "bad-work",
            )
        self.assertIn("bridge.target", caught.exception.codes)
        self.assertFalse(output.exists())

    def _write_ipa_with_crypt_id(self, source: Path, output: Path, crypt_id: int) -> None:
        """Flip crypt_id in place so the input looks like an App Store package."""

        main = self.work / "encrypted-main"
        with ZipFile(source) as archive:
            main.write_bytes(archive.read(patch.MAIN_MEMBER))
        raw = bytearray(main.read_bytes())
        offset = 32  # sizeof(struct mach_header_64)
        while offset < len(raw) - 24:
            command, size = struct.unpack_from("<II", raw, offset)
            if command == 0x2C and size >= 24:  # LC_ENCRYPTION_INFO_64
                struct.pack_into("<I", raw, offset + 16, crypt_id)
                break
            self.assertGreaterEqual(size, 8, "malformed load command")
            offset += size
        else:
            self.fail("LC_ENCRYPTION_INFO_64 is missing")
        main.write_bytes(bytes(raw))
        shutil.copy2(source, output)
        # zipfile.read() resolves the last entry with a given name, so the
        # appended copy wins; the duplicate entry is deliberate
        with warnings.catch_warnings():
            warnings.simplefilter("ignore", UserWarning)
            with ZipFile(output, "a") as archive:
                archive.write(main, patch.MAIN_MEMBER)


def _manifest_with_an_extra_dylib():
    from npabridge.manifest import load_manifest

    manifest = load_manifest(MANIFESTS / "nplayer-3.13.0.json")
    other = Dylib(
        id="other",
        library_version="1.0.0",
        basename="LibOtherBridge.dylib",
        domains=(Domain(id="other", apis=manifest.dylib("libass").domains[0].apis[:1]),),
        extra_sites=(),
    )
    return replace(
        manifest,
        dylibs=manifest.dylibs + (other,),
        default_dylibs=manifest.default_dylibs + ("other",),
    )


if __name__ == "__main__":
    unittest.main()
