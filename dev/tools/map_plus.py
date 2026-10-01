"""Map the standard nPlayer site table onto a nPlayer Plus executable.

Plus is the same app lineage as the standard build but a different
compilation with reordered functions, so the manifest's addresses cannot be
transplanted. This dev-only tool re-derives them from the two executables:

* function pairing by exact and branch-normalized instruction fingerprints,
* byte-window anchors for a monotonic address map,
* call-site resolution by branch target and surrounding bytes,
* every result checked with the same rule `macho.preflight` applies.

It only reads binaries and writes one manifest; it never runs while patching.
"""

from __future__ import annotations

import argparse
import bisect
import hashlib
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

import lief


IMAGEBASE = 0x100000000
TEXT = "__text"
STUBS = "__stubs"
WINDOW = 16
STEP = 4
GAP_MAX = 0x1000
STUB_LENGTH = 12


def _mask(word: int) -> int:
    """Zero the displacement of instructions that encode an address."""

    if word & 0x7C000000 == 0x14000000:  # B / BL
        return word & 0xFC000000
    if word & 0x9F000000 in (0x90000000, 0x10000000):  # ADRP / ADR
        return word & 0x9F00001F
    if word & 0x3B000000 == 0x18000000:  # LDR literal
        return word & ~0x00FFFFE0 & 0xFFFFFFFF
    return word


def _digest(data: bytes) -> bytes:
    return hashlib.blake2b(data, digest_size=8).digest()


def _word(raw: bytes, offset: int) -> int:
    return int.from_bytes(raw[offset : offset + 4], "little")


def _branch_target(raw: bytes, offset: int) -> int:
    word = _word(raw, offset)
    displacement = word & 0x03FFFFFF
    if displacement & 0x02000000:
        displacement -= 0x04000000
    return offset + (displacement << 2)


def _branch_opcode(word: int) -> int:
    opcode = word & 0xFC000000
    if opcode not in (0x14000000, 0x94000000):
        raise ValueError(f"instruction {word:#010x} is neither B nor BL")
    return opcode


def _stub_slot(raw: bytes, offset: int) -> int | None:
    """The GOT slot a `adrp x16; ldr x16, [x16, #imm]; br x16` stub loads."""

    first = _word(raw, offset)
    second = _word(raw, offset + 4)
    third = _word(raw, offset + 8)
    if third != 0xD61F0200:  # br x16
        return None
    if first & 0x9F00001F != 0x90000010:  # adrp x16
        return None
    if second & 0xFFC003FF != 0xF9400210:  # ldr x16, [x16, #imm]
        return None
    immediate = (((first >> 5) & 0x7FFFF) << 2) | ((first >> 29) & 3)
    if immediate & (1 << 20):
        immediate -= 1 << 21
    page = ((offset + IMAGEBASE) & ~0xFFF) + (immediate << 12)
    return page + ((second >> 10) & 0xFFF) * 8


@dataclass(frozen=True)
class Section:
    name: str
    start: int
    end: int


@dataclass
class Image:
    name: str
    raw: bytes
    text: Section
    stubs: Section | None
    func_starts: tuple[int, ...]
    symbols: dict[int, str]

    @classmethod
    def load(cls, path: Path) -> "Image":
        container = lief.MachO.parse(str(path))
        binary = list(container)[0]
        raw = Path(path).read_bytes()
        sections: dict[str, Section] = {}
        for segment in binary.segments:
            for section in segment.sections:
                sections[str(section.name)] = Section(
                    str(section.name),
                    int(section.offset),
                    int(section.offset) + int(section.size),
                )
        if TEXT not in sections:
            raise ValueError(f"{path} carries no __TEXT,__text")
        starts = tuple(
            sorted(
                int(value)
                for value in binary.function_starts.functions
                if sections[TEXT].start <= int(value) < sections[TEXT].end
            )
        )
        symbols = {
            int(item.address): str(item.symbol.name).lstrip("_")
            for item in binary.bindings
        }
        return cls(
            name=path.name,
            raw=raw,
            text=sections[TEXT],
            stubs=sections.get(STUBS),
            func_starts=starts,
            symbols=symbols,
        )

    def stub_for(self, symbol: str) -> int | None:
        """The lazy stub whose GOT slot binds ``symbol``, as a VM address."""

        if self.stubs is None:
            raise ValueError(f"{self.name} carries no __TEXT,__stubs")
        hits = []
        for offset in range(self.stubs.start, self.stubs.end, STUB_LENGTH):
            slot = _stub_slot(self.raw, offset)
            if slot is None:
                continue
            if self.symbols.get(slot) == symbol:
                hits.append(offset + IMAGEBASE)
        if len(hits) != 1:
            return None
        return hits[0]

    def function_at(self, offset: int) -> tuple[int, int] | None:
        index = bisect.bisect_right(self.func_starts, offset) - 1
        if index < 0:
            return None
        start = self.func_starts[index]
        end = (
            self.func_starts[index + 1]
            if index + 1 < len(self.func_starts)
            else self.text.end
        )
        return (start, end) if start <= offset < end else None

    def fingerprint(self, start: int, end: int, normalized: bool) -> str:
        digest = hashlib.sha256()
        offset = start
        while offset + 4 <= end:
            word = _word(self.raw, offset)
            digest.update(
                (_mask(word) if normalized else word).to_bytes(4, "little")
            )
            offset += 4
        return digest.hexdigest()


def _unique_windows(image: Image) -> dict[bytes, int | None]:
    index: dict[bytes, int | None] = {}
    for offset in range(image.text.start, image.text.end - WINDOW + 1, STEP):
        key = _digest(image.raw[offset : offset + WINDOW])
        index[key] = None if key in index else offset
    return index


@dataclass
class Anchors:
    offsets: list[int] = field(default_factory=list)
    deltas: list[int] = field(default_factory=list)

    def add(self, source: int, target: int) -> None:
        self.offsets.append(source)
        self.deltas.append(target - source)

    def finish(self) -> None:
        order = sorted(range(len(self.offsets)), key=lambda i: self.offsets[i])
        self.offsets = [self.offsets[i] for i in order]
        self.deltas = [self.deltas[i] for i in order]

    def map(self, offset: int) -> int | None:
        index = bisect.bisect_left(self.offsets, offset)
        left = None
        if index > 0 and offset - self.offsets[index - 1] <= GAP_MAX:
            left = index - 1
        if index < len(self.offsets) and self.offsets[index] == offset:
            return offset + self.deltas[index]
        right = None
        if index < len(self.offsets) and self.offsets[index] - offset <= GAP_MAX:
            right = index
        if left is not None and right is not None and self.deltas[left] == self.deltas[right]:
            return offset + self.deltas[left]
        return None


def build_anchors(standard: Image, plus: Image) -> Anchors:
    anchors = Anchors()
    # Function-level anchors from unique exact/normalized fingerprints.
    plus_functions: dict[str, list[tuple[int, int]]] = {}
    for index, start in enumerate(plus.func_starts):
        end = (
            plus.func_starts[index + 1]
            if index + 1 < len(plus.func_starts)
            else plus.text.end
        )
        plus_functions.setdefault(plus.fingerprint(start, end, True), []).append(
            (start, end)
        )
    for index, start in enumerate(standard.func_starts):
        end = (
            standard.func_starts[index + 1]
            if index + 1 < len(standard.func_starts)
            else standard.text.end
        )
        matches = plus_functions.get(standard.fingerprint(start, end, True))
        if matches is not None and len(matches) == 1:
            anchors.add(start, matches[0][0])
    # Byte-window anchors for the regions between functions.
    plus_windows = _unique_windows(plus)
    for key, offset in _unique_windows(standard).items():
        if offset is None:
            continue
        target = plus_windows.get(key)
        if target is not None:
            anchors.add(offset, target)
    anchors.finish()
    return anchors


@dataclass
class Mapper:
    standard: Image
    plus: Image
    anchors: Anchors

    def offset(self, offset: int) -> int | None:
        return self.anchors.map(offset)

    def old_target(self, address: int) -> int | None:
        offset = address - IMAGEBASE
        mapped = self.offset(offset)
        if mapped is None:
            return None
        return mapped + IMAGEBASE

    def call_site(self, address: int, target: int | None) -> int | None:
        if target is None:
            return None
        offset = address - IMAGEBASE
        mapped = self.offset(offset)
        if mapped is not None:
            word = _word(self.plus.raw, mapped)
            if _branch_opcode(word) in (0x14000000, 0x94000000):
                if _branch_target(self.plus.raw, mapped) == target - IMAGEBASE:
                    return mapped + IMAGEBASE
        # fall back to a unique context match around the site
        for before, after in ((16, 16), (12, 12), (20, 20), (8, 16), (16, 8)):
            if offset - before < 0:
                continue
            prefix = self.standard.raw[offset - before : offset]
            suffix = self.standard.raw[offset + 4 : offset + 4 + after]
            found = self._context(prefix, suffix, target - IMAGEBASE)
            if found is not None:
                return found + IMAGEBASE
        return None

    def _context(self, prefix: bytes, suffix: bytes, target: int) -> int | None:
        hits: list[int] = []
        cursor = self.plus.raw.find(prefix)
        while cursor != -1 and len(hits) < 64:
            candidate = cursor + len(prefix)
            if (
                _branch_opcode(_word(self.plus.raw, candidate)) in (0x14000000, 0x94000000)
                and _branch_target(self.plus.raw, candidate) == target
                and self.plus.raw[candidate + 4 : candidate + 4 + len(suffix)] == suffix
            ):
                hits.append(candidate)
            cursor = self.plus.raw.find(prefix, cursor + 1)
        return hits[0] if len(hits) == 1 else None

    def exact_word(self, address: int, word: int) -> int | None:
        offset = address - IMAGEBASE
        mapped = self.offset(offset)
        if mapped is not None and _word(self.plus.raw, mapped) == word:
            return mapped + IMAGEBASE
        return None


def _hex(value: int) -> str:
    return f"0x{value:X}"


def map_manifest(
    standard_manifest: dict,
    mapper: Mapper,
    plus_pin: str,
) -> tuple[dict, dict]:
    manifest = json.loads(json.dumps(standard_manifest))
    manifest["main_pin_sha256"] = plus_pin
    resolved: list[str] = []
    unresolved: list[str] = []

    for domain in manifest["domains"]:
        for api in domain["apis"]:
            symbol = api["symbol"]
            target = mapper.old_target(int(api["old_target"], 0))
            if target is None:
                unresolved.append(f"old_target {domain['id']}/{symbol}")
                api["old_target"] = None
            else:
                api["old_target"] = _hex(target)
            new_sites = []
            for site in api["call_sites"]:
                mapped = mapper.call_site(int(site, 0), target)
                if mapped is None:
                    unresolved.append(f"call_site {domain['id']}/{symbol} {site}")
                else:
                    new_sites.append(_hex(mapped))
                    resolved.append(f"call_site {domain['id']}/{symbol} {site}")
            api["call_sites"] = new_sites

    for dylib in manifest["dylibs"]:
        for site in dylib.get("extra_sites", []):
            mapped = mapper.exact_word(int(site["site"], 0), int(site["expected"], 0))
            if mapped is None:
                unresolved.append(f"extra {dylib['id']} {site['site']}")
            else:
                site["site"] = _hex(mapped)
                resolved.append(f"extra {dylib['id']} {site['site']}")
        callback = dylib.get("callback")
        if callback is not None:
            mapped = mapper.old_target(int(callback["app_callback"], 0))
            if mapped is None:
                unresolved.append(f"callback {dylib['id']}")
            else:
                callback["app_callback"] = _hex(mapped)

    for site in manifest.get("main_sites", []):
        mapped = mapper.exact_word(int(site["site"], 0), int(site["expected"], 0))
        if mapped is None:
            unresolved.append(f"main_site {site['site']}")
        else:
            site["site"] = _hex(mapped)

    for name, symbol in (("dlsym_stub", "dlsym"), ("dladdr_stub", "dladdr")):
        mapped = mapper.plus.stub_for(symbol)
        if mapped is None:
            unresolved.append(name)
            continue
        offset = mapped - IMAGEBASE
        manifest[name] = _hex(mapped)
        manifest[f"{name}_thunk"] = mapper.plus.raw[
            offset : offset + STUB_LENGTH
        ].hex()

    report = {
        "resolved": len(resolved),
        "unresolved": unresolved,
    }
    return manifest, report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--standard-manifest", type=Path, required=True)
    parser.add_argument("--standard-main", type=Path, required=True)
    parser.add_argument("--plus-main", type=Path, required=True)
    parser.add_argument("--plus-pin", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, default=None)
    arguments = parser.parse_args(argv)

    standard = Image.load(arguments.standard_main)
    plus = Image.load(arguments.plus_main)
    mapper = Mapper(standard, plus, build_anchors(standard, plus))
    data = json.loads(arguments.standard_manifest.read_text(encoding="utf-8"))
    manifest, report = map_manifest(data, mapper, arguments.plus_pin)
    report["resolved_count"] = report.pop("resolved")
    report["unresolved_count"] = len(report["unresolved"])
    if arguments.report is not None:
        arguments.report.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    summary = {key: value for key, value in report.items() if key != "unresolved"}
    print(json.dumps(summary, indent=2, sort_keys=True))
    if report["unresolved"]:
        for item in report["unresolved"]:
            print("UNRESOLVED:", item)
        print(f"refusing to write {arguments.output}: unresolved sites remain")
        return 1
    arguments.output.write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
