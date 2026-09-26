import struct
import unittest
from dataclasses import replace
from pathlib import Path
from zipfile import ZipFile

from npabridge.macho import (
    IPA_MEMBER,
    SEGMENT_DATA,
    SEGMENT_TEXT,
    parse,
    phase_a,
    phase_b,
    section_bytes,
    snapshot,
)
from npabridge.manifest import APIBinding, Domain, Dylib, ExtraSite, load_manifest
from npabridge.payload import PayloadLayout, assemble_payload, data_size


from support import SOURCE_IPA

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = load_manifest(ROOT / "manifests/nplayer-3.13.0.json")
UNITS = MANIFEST.units()
BUILD = ROOT / "build" / "macho"
NOP_WORD = 0xD503201F


def _spare_bl(baseline: Path, taken: set[int], forbidden_targets: set[int]):
    """The first real BL in __text that no unit claims."""

    binary = parse(baseline)
    content = section_bytes(binary)["__TEXT,__text"]
    base = snapshot(binary).section_vas["__TEXT,__text"]
    for offset in range(0, len(content) - 3, 4):
        word = struct.unpack_from("<I", content, offset)[0]
        if word & 0xFC000000 != 0x94000000:
            continue
        displacement = (word & 0x03FFFFFF) << 2
        if displacement & (1 << 27):
            displacement -= 1 << 28
        site = base + offset
        if site in taken or site + 4 in taken:
            continue
        if site + displacement in forbidden_targets:
            continue
        guard = struct.unpack_from("<I", content, offset + 4)[0]
        return site, site + displacement, site + 4, guard
    raise AssertionError("no spare BL in __TEXT,__text")


class MachOTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SOURCE_IPA.is_file():
            raise unittest.SkipTest("source IPA is not present")
        BUILD.mkdir(parents=True, exist_ok=True)
        cls.baseline = BUILD / "test-clean-main"
        with ZipFile(SOURCE_IPA) as archive:
            cls.baseline.write_bytes(archive.read(IPA_MEMBER))
        cls.layout = BUILD / "test-phase-a"
        cls.patched = BUILD / "test-phase-b"
        cls.phase_a_report = phase_a(cls.baseline, cls.layout, MANIFEST, UNITS)
        cls.phase_b_report = phase_b(cls.layout, cls.patched, MANIFEST, UNITS)

    def test_phase_a_freezes_the_payload_segments(self):
        report = self.phase_a_report
        self.assertEqual(report["reserved_text"], 71184)
        self.assertEqual(
            [name for name, _ in report["dylib_ordinals"][-2:]],
            [
                MANIFEST.dylib("libass").path,
                MANIFEST.dylib("ffmpeg-full").path,
            ],
        )
        after = parse(self.layout)
        text = after.get_segment(SEGMENT_TEXT)
        data = after.get_segment(SEGMENT_DATA)
        self.assertEqual(int(text.virtual_address), report["text_vmaddr"])
        self.assertEqual(int(data.virtual_address), report["data_vmaddr"])
        self.assertEqual(after.segments[-1].name, "__LINKEDIT")

    def test_phase_a_preserves_semantics(self):
        before = snapshot(parse(self.baseline))
        after = snapshot(parse(self.layout))
        self.assertEqual(before.section_vas, after.section_vas)
        self.assertEqual(before.entrypoint, after.entrypoint)
        self.assertEqual(before.bind_targets, after.bind_targets)
        self.assertEqual(before.lazy_targets, after.lazy_targets)
        self.assertEqual(before.export_symbols, after.export_symbols)
        self.assertEqual(before.dylib_ordinals, after.dylib_ordinals[:-2])
        moved = {
            name
            for name, address in before.segment_vas.items()
            if after.segment_vas.get(name) != address
        }
        self.assertEqual(moved, {"__LINKEDIT"})

    def test_phase_b_changes_only_the_frozen_sites(self):
        self.assertEqual(self.phase_b_report["patched_call_sites"], 485)
        self.assertEqual(self.phase_b_report["extra_sites"], [0x100A0392C, 0x100ACBC14])
        before = parse(self.layout)
        after = parse(self.patched)
        self.assertEqual(snapshot(before).segment_vas, snapshot(after).segment_vas)
        self.assertEqual(snapshot(before).section_vas, snapshot(after).section_vas)
        self.assertEqual(self.layout.stat().st_size, self.patched.stat().st_size)
        old_sections = section_bytes(before)
        new_sections = section_bytes(after)
        changed = 0
        for name, old in old_sections.items():
            if name == "__TEXT,__text":
                changed += sum(
                    1
                    for index in range(0, len(old) - 3, 4)
                    if old[index : index + 4] != new_sections[name][index : index + 4]
                )
            else:
                self.assertEqual(old, new_sections[name], name)
        self.assertEqual(changed, 487)

    def test_phase_b_writes_the_assembled_payload(self):
        layout = parse(self.layout)
        text = layout.get_segment(SEGMENT_TEXT)
        data = layout.get_segment(SEGMENT_DATA)
        payload = assemble_payload(
            PayloadLayout(
                text_vmaddr=int(text.virtual_address),
                data_vmaddr=int(data.virtual_address),
            ),
            MANIFEST,
            MANIFEST.target_abi,
            UNITS,
        )
        raw = self.patched.read_bytes()
        start = int(text.file_offset)
        self.assertEqual(raw[start : start + len(payload.text)], payload.text)
        blob = raw[int(data.file_offset) : int(data.file_offset) + data_size(UNITS)]
        self.assertEqual(blob, payload.data)

    def test_a_manifest_with_wrong_extra_site_guard_is_rejected(self):
        broken = replace(
            MANIFEST,
            dylibs=(
                replace(
                    MANIFEST.dylib("libass"),
                    extra_sites=(
                        ExtraSite(0x100A0392C, 0x35000149, NOP_WORD),
                        MANIFEST.dylib("libass").extra_sites[1],
                    ),
                ),
            ),
            default_dylibs=("libass",),
        )
        with self.assertRaises(ValueError) as caught:
            phase_a(
                self.baseline,
                BUILD / "rejected-phase-a",
                broken,
                broken.units(),
            )
        self.assertIn("0x100a0392c", str(caught.exception))

    def test_extra_sites_belong_to_their_own_dylib(self):
        """Selecting one dylib must not touch another dylib's extra sites."""

        taken = {
            site for unit in UNITS for api in unit.apis for site in api.call_sites
        } | {0x100A0392C, 0x100ACBC14}
        forbidden = {api.old_target for unit in UNITS for api in unit.apis}
        call_site, old_target, extra_site, guard = _spare_bl(
            self.baseline, taken, forbidden
        )
        other = Dylib(
            id="other",
            library_version="1.0.0",
            basename="LibOtherBridge.dylib",
            domains=(
                Domain(
                    id="synthetic",
                    apis=(APIBinding("npa_synthetic_one", (call_site,), old_target),),
                ),
            ),
            extra_sites=(ExtraSite(extra_site, guard, NOP_WORD),),
        )
        manifest = replace(MANIFEST, dylibs=MANIFEST.dylibs + (other,))
        units = manifest.units(("other",))

        layout = BUILD / "other-phase-a"
        patched = BUILD / "other-phase-b"
        phase_a(self.baseline, layout, manifest, units)
        report = phase_b(layout, patched, manifest, units)

        self.assertEqual(report["patched_call_sites"], 1)
        self.assertEqual(report["extra_sites"], [extra_site])
        before = parse(layout)
        after = parse(patched)
        changed = set()
        old_text = section_bytes(before)["__TEXT,__text"]
        new_text = section_bytes(after)["__TEXT,__text"]
        base = snapshot(before).section_vas["__TEXT,__text"]
        for index in range(0, len(old_text) - 3, 4):
            if old_text[index : index + 4] != new_text[index : index + 4]:
                changed.add(base + index)
        self.assertEqual(changed, {call_site, extra_site})
        for site in MANIFEST.dylib("libass").extra_sites:
            with self.subTest(site=site.site):
                offset = int(after.virtual_address_to_offset(site.site))
                actual = struct.unpack_from("<I", patched.read_bytes(), offset)[0]
                self.assertEqual(actual, site.expected)


if __name__ == "__main__":
    unittest.main()
