"""Two-phase Mach-O rewrite that installs the atomic libass dispatch.

Phase A lets LIEF rebuild the load commands and segment layout once, adds
the weak bridge dependency and two placeholder segments, and freezes the
final virtual addresses. Phase B never rebuilds the file: it reparses the
frozen layout and writes the assembled payload plus the sixteen BL and
two NOP patches at their final addresses.
"""

from __future__ import annotations

import hashlib
import struct
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence
from zipfile import ZipFile

import lief

from .manifest import Manifest, Unit, branch_opcode, encode_branch
from .payload import PayloadLayout, assemble_payload, data_size, measure_payload
from .target_abi import TargetABI


TARGET = "arm64-apple-ios13.0"
SEGMENT_TEXT = "__NPATCH_TEXT"
SEGMENT_DATA = "__NPATCH_DATA"
LINKEDIT = "__LINKEDIT"
PAGE = 0x4000
PROTECTION_RX = 5
PROTECTION_RW = 3
IPA_MEMBER = "Payload/nPlayer.app/nPlayer"
STUB_THUNKS = {
    0x1011362CC: bytes.fromhex("302f00f0107640f900021fd6"),
    0x10113629C: bytes.fromhex("302f00f0106640f900021fd6"),
}


@dataclass(frozen=True)
class SemanticSnapshot:
    segment_vas: dict[str, int]
    section_vas: dict[str, int]
    entrypoint: int
    dylib_ordinals: tuple[tuple[str, int], ...]
    bind_targets: tuple[tuple[str, str, int], ...]
    lazy_targets: tuple[tuple[str, str, int], ...]
    export_symbols: tuple[tuple[str, int], ...]


class ParsedMachO:
    """A parsed Mach-O that keeps its LIEF container alive.

    LIEF binaries are views into the parse result; letting the container be
    collected leaves dangling references that crash on the next access.
    """

    __slots__ = ("_container", "lief")

    def __init__(self, container: Any, binary: Any) -> None:
        self._container = container
        self.lief = binary

    def __getattr__(self, name: str) -> Any:
        return getattr(self.lief, name)


def parse(path: Path) -> ParsedMachO:
    """Parse one Mach-O file, keeping the LIEF container alive."""

    container = lief.MachO.parse(str(path))
    binaries = list(container) if container is not None else []
    if len(binaries) != 1:
        raise ValueError(f"expected one Mach-O binary in {path}")
    return ParsedMachO(container, binaries[0])


def extract_clean_main(input_path: Path, destination: Path) -> Path:
    """Resolve the patch input, extracting the frozen IPA member if needed."""

    if input_path.suffix != ".ipa":
        return input_path
    destination.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(input_path) as archive:
        destination.write_bytes(archive.read(IPA_MEMBER))
    return destination


def _binding_targets(binary: Any, binding_class: str) -> tuple[tuple[str, str, int], ...]:
    seen = set()
    for item in binary.bindings:
        if str(item.binding_class).rsplit(".", 1)[-1] != binding_class:
            continue
        library = str(item.library.name) if item.has_library else ""
        seen.add((str(item.symbol), library, int(item.address)))
    return tuple(sorted(seen))


def snapshot(binary: Any) -> SemanticSnapshot:
    return SemanticSnapshot(
        segment_vas={segment.name: int(segment.virtual_address) for segment in binary.segments},
        section_vas={
            f"{segment.name},{section.name}": int(section.virtual_address)
            for segment in binary.segments
            for section in segment.sections
        },
        entrypoint=int(binary.entrypoint),
        dylib_ordinals=tuple(
            (str(library.name), index + 1) for index, library in enumerate(binary.libraries)
        ),
        bind_targets=_binding_targets(binary, "STANDARD"),
        lazy_targets=_binding_targets(binary, "LAZY"),
        export_symbols=tuple(
            sorted(
                (str(function.name), int(function.address))
                for function in binary.exported_functions
            )
        ),
    )


def section_bytes(binary: Any) -> dict[str, bytes]:
    return {
        f"{segment.name},{section.name}": bytes(section.content)
        for segment in binary.segments
        for section in segment.sections
    }


def exported_symbols(binary: Any) -> list[str]:
    return sorted({str(symbol.name) for symbol in binary.exported_symbols})


def imported_symbols(binary: Any) -> list[str]:
    return sorted({str(symbol.name) for symbol in binary.imported_symbols})


def section_size(binary: Any, name: str) -> int:
    return sum(int(section.size) for section in binary.sections if str(section.name) == name)


def enum_name(value: object) -> str:
    return str(value).rsplit(".", 1)[-1].upper()


def version_tuple(value: object) -> list[int]:
    parts = tuple(int(part) for part in value)
    if len(parts) > 3:
        raise ValueError(f"invalid Mach-O version: {parts}")
    return list(parts + (0,) * (3 - len(parts)))


def xcrun_find(name: str) -> str:
    result = subprocess.run(
        ["/usr/bin/xcrun", "--sdk", "iphoneos", "--find", name],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=True,
    )
    path = result.stdout.strip()
    if not path:
        raise RuntimeError(f"xcrun could not find {name}")
    return path


def dependency_lines(path: Path) -> list[str]:
    """Dependencies and the self install name, in otool -L order."""

    binary = parse(path)
    return [str(library.name) for library in binary.libraries]


def install_name(path: Path) -> str:
    binary = parse(path)
    if not binary.has(lief.MachO.LoadCommand.TYPE.ID_DYLIB):
        raise ValueError(f"no LC_ID_DYLIB in {path}")
    for command in binary.commands:
        if command.command == lief.MachO.LoadCommand.TYPE.ID_DYLIB:
            return str(command.name)
    raise ValueError(f"LC_ID_DYLIB is unreadable in {path}")


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _round_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def _word(binary: Any, address: int) -> int:
    content = bytes(binary.get_content_from_virtual_address(address, 4))
    if len(content) != 4:
        raise ValueError(f"unable to read four bytes at {address:#x}")
    return struct.unpack("<I", content)[0]


def provisional_layout(binary: Any) -> PayloadLayout:
    """Addresses LIEF is expected to use for the two placeholder segments."""

    body = [segment for segment in binary.segments if segment.name != LINKEDIT]
    last = body[-1]
    text_vmaddr = (int(last.virtual_address) + int(last.virtual_size) + PAGE - 1) & ~(PAGE - 1)
    data_vmaddr = text_vmaddr + PAGE
    return PayloadLayout(text_vmaddr=text_vmaddr, data_vmaddr=data_vmaddr)


def selected_extra_sites(manifest: Manifest, units: Sequence[Unit]):
    """Every word-level site this selection rewrites in the main binary.

    The selected dylibs contribute the sites they need, and the manifest's own
    app-level `main_sites` are always included.
    """

    return manifest.extra_sites([unit.dylib_id for unit in units]) + manifest.main_sites


def preflight(
    input_path: Path,
    manifest: Manifest,
    units: Sequence[Unit],
    digest: str | None = None,
) -> None:
    """Reject anything that is not the frozen clean main."""

    data = input_path.read_bytes()
    actual = hashlib.sha256(data).hexdigest()
    if actual != (digest or manifest.main_sha256):
        raise ValueError(f"input SHA-256 does not match the frozen baseline: {actual}")
    binary = parse(input_path)
    if not binary.has_encryption_info or int(binary.encryption_info.crypt_id) != 0:
        raise ValueError("input is encrypted")
    if not binary.has_code_signature:
        raise ValueError("input has no LC_CODE_SIGNATURE to replace")
    if binary.segments[-1].name != LINKEDIT:
        raise ValueError("__LINKEDIT is not the final segment")
    for name in (SEGMENT_TEXT, SEGMENT_DATA):
        if binary.has_segment(name):
            raise ValueError(f"input already carries {name}")
    for library in binary.libraries:
        if str(library.name) in {manifest.dylib(unit.dylib_id).path for unit in units}:
            raise ValueError("input already loads the bridge")
    if int(binary.available_command_space) < 128:
        raise ValueError("no load-command space for the weak dependency")
    for extra in selected_extra_sites(manifest, units):
        if _word(binary, extra.site) != extra.expected:
            raise ValueError(f"unexpected instruction at extra site {extra.site:#x}")
    for stub, thunk in STUB_THUNKS.items():
        content = bytes(binary.get_content_from_virtual_address(stub, len(thunk)))
        if content != thunk:
            raise ValueError(f"dynamic stub at {stub:#x} is not the frozen thunk")
    for unit in units:
        for api in unit.apis:
            for site in api.call_sites:
                actual = _word(binary, site)
                expected = encode_branch(branch_opcode(actual), site, api.old_target)
                if actual != expected:
                    raise ValueError(
                        f"call site {site:#x} does not branch to {api.old_target:#x}: "
                        f"{actual:#010x} != {expected:#010x}"
                    )


def _segment(name: str, vmaddr: int, offset: int, content: bytes, protection: int) -> Any:
    segment = lief.MachO.SegmentCommand(name)
    segment.virtual_address = vmaddr
    segment.file_offset = offset
    segment.content = list(content)
    segment.init_protection = protection
    segment.max_protection = protection
    return segment


def phase_a(
    input_path: Path,
    output_path: Path,
    manifest: Manifest,
    units: Sequence[Unit],
    reserved_text: int | None = None,
    target_abi: TargetABI | None = None,
) -> dict[str, Any]:
    """Rebuild the load commands and freeze the payload segment layout."""

    abi = target_abi or manifest.target_abi
    preflight(input_path, manifest, units)
    binary = parse(input_path)
    provisional = provisional_layout(binary)
    measured = measure_payload(provisional, manifest, abi, units)
    if reserved_text is not None and reserved_text != measured:
        raise ValueError(
            f"reserved payload size {reserved_text} does not match measured {measured}"
        )
    reserved_text = measured
    if binary.has_code_signature:
        binary.remove_signature()
    for dylib_id in dict.fromkeys(unit.dylib_id for unit in units):
        binary.add(lief.MachO.DylibCommand.weak_lib(manifest.dylib(dylib_id).path))
    text_offset = _round_up(
        int(binary.segments[-1].file_offset) + int(binary.segments[-1].file_size), PAGE
    )
    binary.add(
        _segment(
            SEGMENT_TEXT,
            provisional.text_vmaddr,
            text_offset,
            b"\x00" * reserved_text,
            PROTECTION_RX,
        )
    )
    binary.add(
        _segment(
            SEGMENT_DATA,
            provisional.data_vmaddr,
            text_offset + _round_up(reserved_text, PAGE),
            b"\x00" * data_size(units),
            PROTECTION_RW,
        )
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    binary.write(str(output_path))
    del binary
    after = parse(output_path)
    text = after.get_segment(SEGMENT_TEXT)
    data = after.get_segment(SEGMENT_DATA)
    _require(text is not None and data is not None, "placeholder segments are missing")
    layout = PayloadLayout(
        text_vmaddr=int(text.virtual_address),
        data_vmaddr=int(data.virtual_address),
    )
    _require(
        measure_payload(layout, manifest, abi, units) == reserved_text,
        "payload size depends on the final addresses",
    )
    state = snapshot(after)
    return {
        "baseline": str(input_path),
        "layout": str(output_path),
        "text_vmaddr": layout.text_vmaddr,
        "data_vmaddr": layout.data_vmaddr,
        "reserved_text": reserved_text,
        "text_file_offset": int(text.file_offset),
        "data_file_offset": int(data.file_offset),
        "linkedit_vmaddr": state.segment_vas[LINKEDIT],
        "dylib_ordinals": [list(item) for item in state.dylib_ordinals],
    }


def write_equal_length(buffer: bytearray, offset: int, payload: bytes) -> None:
    end = offset + len(payload)
    if offset < 0 or end > len(buffer):
        raise ValueError(f"write at {offset:#x} of {len(payload)} bytes leaves the file")
    buffer[offset:end] = payload


def patch_branch(opcode: int, site: int, target: int) -> bytes:
    return struct.pack("<I", encode_branch(opcode, site, target))


def phase_b(
    layout_path: Path,
    output_path: Path,
    manifest: Manifest,
    units: Sequence[Unit],
    target_abi: TargetABI | None = None,
) -> dict[str, Any]:
    """Write the payload and the branch patches into the frozen layout."""

    abi = target_abi or manifest.target_abi
    binary = parse(layout_path)
    text = binary.get_segment(SEGMENT_TEXT)
    data = binary.get_segment(SEGMENT_DATA)
    if text is None or data is None:
        raise ValueError("layout has no placeholder segments")
    layout = PayloadLayout(
        text_vmaddr=int(text.virtual_address),
        data_vmaddr=int(data.virtual_address),
    )
    payload = assemble_payload(layout, manifest, abi, units)
    if len(payload.text) > int(text.file_size):
        raise ValueError(
            f"assembled payload is {len(payload.text)} bytes, "
            f"segment holds {int(text.file_size)}"
        )
    if int(text.file_offset) + len(payload.text) > int(data.file_offset):
        raise ValueError("assembled payload spills into the data segment")
    if len(payload.data) > int(data.file_size):
        raise ValueError("assembled data blob does not fit its segment")

    buffer = bytearray(layout_path.read_bytes())
    writes: list[tuple[int, int]] = [
        (int(text.file_offset), len(payload.text)),
        (int(data.file_offset), len(payload.data)),
    ]
    write_equal_length(buffer, int(text.file_offset), payload.text)
    write_equal_length(buffer, int(data.file_offset), payload.data)
    for unit in units:
        for api in unit.apis:
            for site in api.call_sites:
                offset = int(binary.virtual_address_to_offset(site))
                actual = struct.unpack_from("<I", buffer, offset)[0]
                opcode = branch_opcode(actual)
                if actual != encode_branch(opcode, site, api.old_target):
                    raise ValueError(f"call site {site:#x} is no longer the frozen branch")
                target = payload.symbols[f"veneer_{api.symbol}"]
                write_equal_length(buffer, offset, patch_branch(opcode, site, target))
                writes.append((offset, 4))
    extra_sites = selected_extra_sites(manifest, units)
    for extra in extra_sites:
        offset = int(binary.virtual_address_to_offset(extra.site))
        if struct.unpack_from("<I", buffer, offset)[0] != extra.expected:
            raise ValueError(
                f"extra site {extra.site:#x} no longer holds the frozen word"
            )
        write_equal_length(buffer, offset, struct.pack("<I", extra.replacement))
        writes.append((offset, 4))

    _assert_intended_writes(layout_path.read_bytes(), bytes(buffer), writes)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_bytes(bytes(buffer))
    del binary
    return {
        "layout": str(layout_path),
        "output": str(output_path),
        "patched_call_sites": sum(unit.call_site_count for unit in units),
        "extra_sites": sorted(extra.site for extra in extra_sites),
        "text_vmaddr": layout.text_vmaddr,
        "data_vmaddr": layout.data_vmaddr,
        "payload_text_size": len(payload.text),
        "payload_symbols": len(payload.symbols),
    }


def _assert_intended_writes(before: bytes, after: bytes, writes: list[tuple[int, int]]) -> None:
    """Phase B may only touch the declared ranges."""

    _require(len(before) == len(after), "phase B changed the file size")
    touched = bytearray(len(before))
    for offset, size in writes:
        for index in range(offset, offset + size):
            touched[index] = 1
    for index in range(len(before)):
        if not touched[index]:
            _require(
                before[index] == after[index],
                f"phase B changed byte {index:#x} outside the declared writes",
            )


def sdk_path() -> Path:
    result = subprocess.run(
        ["/usr/bin/xcrun", "--sdk", "iphoneos", "--show-sdk-path"],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=True,
    )
    return Path(result.stdout.strip())
