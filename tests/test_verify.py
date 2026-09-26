import struct
import unittest
from dataclasses import replace
from pathlib import Path
from zipfile import ZipFile

from npabridge import macho
from npabridge.manifest import BL_OPCODE, encode_branch, load_manifest
from npabridge.macho import IPA_MEMBER, parse, phase_a, phase_b
from npabridge.payload import unit_offsets
from npabridge.verify import VerificationError, verify_artifact, verify_bridge

from support import SOURCE_IPA

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = load_manifest(ROOT / "manifests/nplayer-3.13.0.json")
UNITS = MANIFEST.units()
BUILD = ROOT / "build" / "macho"
BRIDGES = {
    "libass": ROOT / "build" / "LibASSBridge.dylib",
    "ffmpeg": ROOT / "build" / "LibFFmpegBridge.dylib",
    "ffmpeg-core": ROOT / "build" / "LibFFmpegCoreBridge.dylib",
    "ffmpeg-full": ROOT / "build" / "LibFFmpegFullBridge.dylib",
    "ffmpeg-demux": ROOT / "build" / "LibFFmpegDemuxBridge.dylib",
}


class VerifyTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SOURCE_IPA.is_file():
            raise unittest.SkipTest("source IPA is not present")
        if not BRIDGES["libass"].is_file():
            raise unittest.SkipTest("LibASSBridge.dylib is not built")
        BUILD.mkdir(parents=True, exist_ok=True)
        cls.baseline = BUILD / "verify-clean-main"
        with ZipFile(SOURCE_IPA) as archive:
            cls.baseline.write_bytes(archive.read(IPA_MEMBER))
        cls.layout = BUILD / "verify-phase-a"
        cls.patched = BUILD / "verify-phase-b"
        phase_a(cls.baseline, cls.layout, MANIFEST, UNITS)
        phase_b(cls.layout, cls.patched, MANIFEST, UNITS)

    def test_valid_artifact_passes_every_check(self):
        report = verify_artifact(
            self.baseline, self.patched, MANIFEST, UNITS, BRIDGES
        )
        report.require()
        self.assertEqual(report.state_initial, 0)
        self.assertEqual(
            set(report.bridge_sha256s),
            {"LibASSBridge.dylib", "LibFFmpegFullBridge.dylib"},
        )
        for check in report.checks:
            self.assertTrue(check.ok, check)

    def test_wrong_call_site_is_rejected(self):
        mutated = self._mutate(self.patched, "mutated-callsite")
        site = MANIFEST.api("npa_ass_render_frame").call_sites[0]
        binary = parse(mutated)
        offset = int(binary.virtual_address_to_offset(site))
        raw = bytearray(mutated.read_bytes())
        raw[offset : offset + 4] = struct.pack("<I", encode_branch(BL_OPCODE, site, 0x100A00000))
        mutated.write_bytes(bytes(raw))
        report = verify_artifact(
            self.baseline, mutated, MANIFEST, UNITS, BRIDGES
        )
        with self.assertRaises(VerificationError) as caught:
            report.require()
        self.assertIn("main.call_sites", caught.exception.codes)

    def test_non_zero_state_is_rejected(self):
        mutated = self._mutate(self.patched, "mutated-state")
        binary = parse(mutated)
        segment = binary.get_segment(macho.SEGMENT_DATA)
        _, state_offset = unit_offsets(UNITS)[-1]
        raw = bytearray(mutated.read_bytes())
        offset = int(segment.file_offset) + state_offset
        raw[offset : offset + 4] = struct.pack("<I", 1)
        mutated.write_bytes(bytes(raw))
        report = verify_artifact(
            self.baseline, mutated, MANIFEST, UNITS, BRIDGES
        )
        with self.assertRaises(VerificationError) as caught:
            report.require()
        self.assertIn("payload.state", caught.exception.codes)

    def test_restored_extra_site_is_rejected(self):
        mutated = self._mutate(self.patched, "mutated-extra-site")
        binary = parse(mutated)
        raw = bytearray(mutated.read_bytes())
        for extra in MANIFEST.dylib("libass").extra_sites:
            offset = int(binary.virtual_address_to_offset(extra.site))
            raw[offset : offset + 4] = struct.pack("<I", extra.expected)
        mutated.write_bytes(bytes(raw))
        report = verify_artifact(
            self.baseline, mutated, MANIFEST, UNITS, BRIDGES
        )
        with self.assertRaises(VerificationError) as caught:
            report.require()
        self.assertIn("main.extra_sites", caught.exception.codes)
        self.assertIn("main.instructions", caught.exception.codes)

    def test_extra_export_in_the_dylib_is_rejected(self):
        full = MANIFEST.dylib("libass")
        truncated = replace(
            full,
            domains=(
                replace(full.domains[0], apis=full.domains[0].apis[:-1]),
            ),
        )
        report = verify_bridge(BRIDGES["libass"], truncated)
        with self.assertRaises(VerificationError) as caught:
            report.require()
        self.assertIn("bridge.exports", caught.exception.codes)

    def _mutate(self, source: Path, name: str) -> Path:
        destination = source.parent / name
        destination.write_bytes(source.read_bytes())
        return destination


if __name__ == "__main__":
    unittest.main()
