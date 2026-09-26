import struct
import unittest
from dataclasses import FrozenInstanceError, replace
from pathlib import Path

from npabridge.manifest import APIBinding, Domain, Dylib, load_manifest
from npabridge.payload import (
    Payload,
    PayloadLayout,
    _BASENAME_SCAN_SIZE,
    _RESOLVE_DLADDR_CHECK,
    assemble_payload,
    measure_payload,
)
from npabridge.target_abi import TargetABI


ROOT = Path(__file__).resolve().parents[1]
MANIFEST = load_manifest(ROOT / "manifests/nplayer-3.13.0.json")
ABI = TargetABI(
    platform="IOS",
    minos=(13, 0, 0),
    sdk=(26, 2, 0),
    dl_info_size=32,
    dl_info_fname_offset=0,
    dl_info_fbase_offset=8,
    dl_info_sname_offset=16,
    dl_info_saddr_offset=24,
    rtld_default_masked=(1 << 64) - 2,
)

LDAR = 0x88DFFD10      # ldar w16, [x8]
LDAXR = 0x885FFD10     # ldaxr w16, [x8]
STXR = 0x88097D11      # stxr w9, w17, [x8]
STLR = 0x889FFD10      # stlr w16, [x8]
YIELD = 0xD503203F


def _layout(text_vmaddr=0x101200000, data_vmaddr=0x101210000):
    return PayloadLayout(text_vmaddr=text_vmaddr, data_vmaddr=data_vmaddr)


def _two_unit_manifest():
    """A second dylib with one synthetic domain, for the multi-unit layout."""

    other = Dylib(
        id="other",
        library_version="1.0.0",
        basename="LibOtherBridge.dylib",
        domains=(
            Domain(
                id="synthetic",
                apis=(
                    APIBinding("npa_synthetic_one", (0x100A11000,), 0x100C0B060),
                    APIBinding("npa_synthetic_two", (0x100A11004,), 0x100C0A2F4),
                ),
            ),
        ),
    )
    return replace(
        MANIFEST,
        dylibs=MANIFEST.dylibs + (other,),
        default_dylibs=MANIFEST.default_dylibs + ("other",),
    )


def _sign_extend(value, bits):
    sign = 1 << (bits - 1)
    return (value & (sign - 1)) - (value & sign)


def _all_branch_targets(code, layout):
    result = []
    for offset in range(0, len(code) - 3, 4):
        word = struct.unpack_from("<I", code, offset)[0]
        if word & 0xFC000000 in (0x14000000, 0x94000000):
            displacement = _sign_extend((word & 0x03FFFFFF) << 2, 28)
            result.append(
                (offset, layout.text_vmaddr + offset + displacement, word & 0xFC000000)
            )
    return result


def _code_words(code):
    return [
        struct.unpack_from("<I", code, offset)[0]
        for offset in range(0, len(code) - 3, 4)
    ]


def _cmp_w5_immediates(code):
    """Every `cmp w5, #imm` in the encoded payload, in order."""

    result = []
    for word in _code_words(code):
        if word & 0xFFC003FF == 0x710000BF:
            result.append((word >> 10) & 0xFFF)
    return result


def _words_at(code, layout, address, count):
    offset = address - layout.text_vmaddr
    return [
        struct.unpack_from("<I", code, offset + 4 * index)[0]
        for index in range(count)
    ]


def _adrp_target(word, address):
    immlo = (word >> 29) & 0x3
    immhi = (word >> 5) & 0x7FFFF
    page = ((immhi << 2) | immlo) << 12
    if page & (1 << 32):
        page -= 1 << 33
    return (address & ~0xFFF) + page


def _add_immediate(word):
    if word & 0xFFC00000 != 0x91000000:
        raise AssertionError(f"not an ADD immediate: {word:#010x}")
    return (word >> 10) & 0xFFF


def _unsigned_offset(word):
    if word & 0xFFC00000 not in (0xF9400000, 0xF9000000):
        raise AssertionError(f"not an LDR/STR unsigned offset: {word:#010x}")
    return ((word >> 10) & 0xFFF) * 8


def _encoded_slot_address(payload, layout, symbol, kind):
    """The address `adrp/add` + `ldr/str` resolve to for one unit slot access."""

    if kind == "load":
        block = payload.symbols[f"new_{symbol}"]
        words = _words_at(payload.text, layout, block, 3)
        adrp, add, access = words
        adrp_pc = block
    else:
        block = payload.symbols[f"store_{symbol}"]
        words = _words_at(payload.text, layout, block, 4)
        adrp, add, access = words[1], words[2], words[3]
        # adrp is pc-relative: its own address decides the page, and a block
        # that straddles a page boundary puts it on the next one.
        adrp_pc = block + 4
    return (
        _adrp_target(adrp, adrp_pc) + _add_immediate(add) + _unsigned_offset(access)
    )


def _basename_occurrences(immediates, basename):
    expected = [ord(character) for character in basename]
    return sum(
        1
        for index in range(len(immediates) - len(expected) + 1)
        if immediates[index : index + len(expected)] == expected
    )


class PayloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.layout = _layout()
        cls.units = MANIFEST.units()
        cls.apis = [api for unit in cls.units for api in unit.apis]
        cls.payload = assemble_payload(cls.layout, MANIFEST, ABI, cls.units)

    def test_public_dataclasses_are_frozen(self):
        with self.assertRaises(FrozenInstanceError):
            self.payload.text = b"tampered"
        with self.assertRaises(FrozenInstanceError):
            self.layout.text_vmaddr = 1

    def test_measure_matches_text_and_is_va_independent(self):
        other = _layout(0x101300000, 0x101310000)
        self.assertEqual(
            measure_payload(self.layout, MANIFEST, ABI, self.units),
            len(self.payload.text),
        )
        self.assertEqual(
            measure_payload(other, MANIFEST, ABI, self.units), len(self.payload.text)
        )
        other_payload = assemble_payload(other, MANIFEST, ABI, self.units)
        self.assertEqual(len(other_payload.text), len(self.payload.text))
        self.assertEqual(other_payload.data, self.payload.data)

    def test_data_is_one_state_and_slot_table_per_unit(self):
        expected = sum(8 + 8 * unit.symbol_count for unit in self.units)
        self.assertEqual(len(self.payload.data), expected)
        self.assertEqual(self.payload.data, b"\x00" * expected)
        for unit in self.units:
            base = self.payload.symbols[f"state_{unit.id}"] - self.layout.data_vmaddr
            self.assertEqual(
                struct.unpack_from("<II", self.payload.data, base), (0, 0)
            )
            self.assertEqual(
                self.payload.symbols[f"slots_{unit.id}"],
                self.layout.data_vmaddr + base + 8,
            )

    def test_every_unit_has_its_own_state_word_and_slot_table(self):
        units = MANIFEST.units()
        bases = [
            self.payload.symbols[f"state_{unit.id}"] - self.layout.data_vmaddr
            for unit in units
        ]
        self.assertEqual(len(bases), len(set(bases)))
        slots = [self.payload.symbols[f"slots_{unit.id}"] for unit in units]
        self.assertEqual(len(slots), len(set(slots)))

    def test_symbols_cover_every_veneer_and_callsite(self):
        veneers = {
            name: address
            for name, address in self.payload.symbols.items()
            if name.startswith("veneer_")
        }
        self.assertEqual(
            set(veneers), {f"veneer_{api.symbol}" for api in self.apis}
        )
        callsites = {
            name: address
            for name, address in self.payload.symbols.items()
            if name.startswith("callsite_")
        }
        self.assertEqual(
            set(callsites),
            {f"callsite_{site:#x}" for api in self.apis for site in api.call_sites},
        )
        process = self.payload.symbols["veneer_npa_ass_process_data"]
        self.assertEqual(self.payload.symbols["callsite_0x100a0482c"], process)
        self.assertEqual(self.payload.symbols["callsite_0x100a0529c"], process)

    def test_stub_addresses_are_exposed_without_relocation(self):
        self.assertEqual(self.payload.stubs["dlsym"], MANIFEST.dlsym_stub)
        self.assertEqual(self.payload.stubs["dladdr"], MANIFEST.dladdr_stub)

    def test_all_veneer_old_branches_and_stub_calls_are_in_range(self):
        branches = _all_branch_targets(self.payload.text, self.layout)
        old_targets = {api.old_target for api in self.apis}
        self.assertEqual(
            sum(target in old_targets for _, target, _ in branches), len(self.apis)
        )
        self.assertEqual(
            sum(target == MANIFEST.dlsym_stub for _, target, _ in branches), len(self.apis)
        )
        self.assertEqual(
            sum(target == MANIFEST.dladdr_stub for _, target, _ in branches), len(self.apis)
        )
        for offset, target, _ in branches:
            self.assertLessEqual(
                abs(target - (self.layout.text_vmaddr + offset)),
                1 << 27,
                f"branch at {offset:#x} targets {target:#x}",
            )

    def test_free_track_has_exact_old_target_branch(self):
        offset = self.payload.symbols["veneer_npa_ass_free_track"] - self.layout.text_vmaddr
        matches = [
            target
            for branch_offset, target, opcode in _all_branch_targets(
                self.payload.text, self.layout
            )
            if offset <= branch_offset < offset + 0x40 and opcode == 0x14000000
        ]
        self.assertIn(0x100C0A2F4, matches)

    def test_encoded_state_machine_shape(self):
        code_end = self.payload.symbols["string_npa_ass_library_init"] - self.layout.text_vmaddr
        words = _code_words(self.payload.text[:code_end])
        self.assertEqual(words.count(LDAR), len(self.apis) + len(self.units))
        self.assertEqual(words.count(LDAXR), len(self.units))
        self.assertEqual(words.count(STXR), len(self.units))
        self.assertEqual(words.count(STLR), 2 * len(self.units))
        self.assertEqual(words.count(YIELD), len(self.units))

    def test_text_has_no_pointer_literal_or_literal_pool(self):
        code_end = self.payload.symbols["string_npa_ass_library_init"] - self.layout.text_vmaddr
        for offset in range(0, code_end, 4):
            word = struct.unpack_from("<I", self.payload.text, offset)[0]
            self.assertNotIn(
                word & 0xFF800000,
                (0xD2800000, 0xF2800000),
                f"64-bit MOVZ/MOVK literal at {offset:#x}",
            )
            self.assertNotIn(
                word & 0xFF000000,
                (0x18000000, 0x58000000),
                f"LDR literal pool access at {offset:#x}",
            )

    def test_generation_is_deterministic(self):
        again = assemble_payload(self.layout, MANIFEST, ABI, self.units)
        self.assertEqual(again.text, self.payload.text)
        self.assertEqual(again.data, self.payload.data)
        self.assertEqual(again.symbols, self.payload.symbols)
        self.assertEqual(again.stubs, self.payload.stubs)

    def test_out_of_range_branch_is_rejected_before_returning_bytes(self):
        far = _layout(0x201200000, 0x201210000)
        with self.assertRaises(ValueError):
            assemble_payload(far, MANIFEST, ABI, self.units)

    def test_payload_constructor_accepts_declared_interface(self):
        payload = Payload(text=b"text", data=b"data", symbols={}, stubs={})
        self.assertEqual(payload.text, b"text")
        self.assertEqual(payload.data, b"data")
        self.assertEqual(payload.symbols, {})
        self.assertEqual(payload.stubs, {})


class MultiUnitPayloadTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.layout = _layout()
        cls.manifest = _two_unit_manifest()
        cls.units = cls.manifest.units()
        cls.apis = [api for unit in cls.units for api in unit.apis]
        cls.payload = assemble_payload(cls.layout, cls.manifest, ABI, cls.units)

    def test_every_unit_arbitrates_its_own_state(self):
        words = _code_words(self.payload.text)
        units = len(self.units)
        self.assertEqual(words.count(LDAXR), units)
        self.assertEqual(words.count(STXR), units)
        self.assertEqual(words.count(STLR), 2 * units)
        self.assertEqual(words.count(YIELD), units)
        self.assertEqual(words.count(LDAR), len(self.apis) + units)

    def test_each_unit_checks_only_its_own_basename(self):
        immediates = _cmp_w5_immediates(self.payload.text)
        expected: dict[str, int] = {}
        for unit in self.units:
            expected[unit.basename] = expected.get(unit.basename, 0) + unit.symbol_count
        for unit in self.units:
            with self.subTest(unit=unit.id):
                self.assertEqual(
                    _basename_occurrences(immediates, unit.basename),
                    expected[unit.basename],
                )

    def test_every_slot_access_resolves_to_its_own_slot(self):
        for unit_name, unit in [(unit.id, unit) for unit in self.units]:
            for index, api in enumerate(unit.apis):
                expected = self.payload.symbols[f"slots_{unit_name}"] + 8 * index
                for kind in ("load", "store"):
                    with self.subTest(unit=unit_name, symbol=api.symbol, kind=kind):
                        self.assertEqual(
                            _encoded_slot_address(
                                self.payload, self.layout, api.symbol, kind
                            ),
                            expected,
                        )

    def test_data_grows_with_the_extra_unit(self):
        expected = sum(8 + 8 * unit.symbol_count for unit in self.units)
        without = sum(8 + 8 * unit.symbol_count for unit in MANIFEST.units())
        self.assertEqual(len(self.payload.data), expected)
        self.assertEqual(len(self.payload.data), without + 8 + 8 * 2)

    def test_selection_drops_the_unselected_unit(self):
        only_first = self.manifest.units(("libass",))
        payload = assemble_payload(self.layout, self.manifest, ABI, only_first)
        self.assertNotIn("state_other/synthetic", payload.symbols)
        self.assertEqual(len(payload.data), sum(8 + 8 * u.symbol_count for u in only_first))

    def test_resolve_check_treats_dladdr_success_as_success(self):
        """dladdr returns non-zero on success; a zero result is the fallback."""

        for unit in self.units:
            publish_old = self.payload.symbols[f"publish_old_{unit.id}"]
            for api in unit.apis:
                with self.subTest(symbol=api.symbol):
                    resolve = self.payload.symbols[f"resolve_{api.symbol}"]
                    offset = resolve - self.layout.text_vmaddr + _RESOLVE_DLADDR_CHECK
                    word = struct.unpack_from("<I", self.payload.text, offset)[0]
                    self.assertEqual(word & 0xFF00001F, 0x34000000)  # cbz w0
                    displacement = (word >> 5) & 0x7FFFF
                    if displacement & (1 << 18):
                        displacement -= 1 << 19
                    self.assertEqual(
                        resolve + _RESOLVE_DLADDR_CHECK + (displacement << 2),
                        publish_old,
                    )

    def test_basename_scan_reads_the_whole_path(self):
        """The scan must reach the NUL, not stop at the first slash."""

        for unit in self.units:
            for api in unit.apis:
                with self.subTest(symbol=api.symbol):
                    scan = self.payload.symbols[f"basename_scan_{api.symbol}"]
                    suffix = self.payload.symbols[f"basename_suffix_{api.symbol}"]
                    self.assertEqual(suffix - scan, _BASENAME_SCAN_SIZE)


if __name__ == "__main__":
    unittest.main()
