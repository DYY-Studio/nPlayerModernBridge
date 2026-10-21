"""Renderer host offsets generated and verified against supported manifests."""
from __future__ import annotations

import struct
from pathlib import Path

from .manifest import Manifest, load_manifest

SECTION = '__npa_renderer'
RECORD_SIZE = 48

# Dylibs whose call site has to find its own original implementation through
# the caller's image. Each one carries its own read-only table.
RENDERER_DYLIB_IDS = ('renderer-highbit', 'renderer-hdr')

# Every table carries one record per host build and API; verification only
# demands the records of the host being packaged.
HOST_MANIFESTS = ('nplayer-3.13.0.json', 'nplayer-plus-3.13.0.json')


def records(manifest: Manifest, dylib_id: str) -> list[bytes]:
    """One frozen record per API of this dylib in this host build."""

    rows = []
    for domain in manifest.dylib(dylib_id).domains:
        for api in domain.apis:
            if len(api.call_sites) != 1:
                raise ValueError(
                    f'{dylib_id}/{api.symbol} requires one frozen caller'
                )
            rows.append(
                bytes.fromhex(manifest.main_pin_sha256)
                + struct.pack(
                    '<2Q',
                    api.old_target - manifest.imagebase,
                    api.call_sites[0] + 4 - manifest.imagebase,
                )
            )
    if not rows:
        raise ValueError(f'{dylib_id} declares no renderer binding')
    return rows


def generate_header(directory: Path, output: Path, dylib_id: str) -> None:
    rows = []
    for name in HOST_MANIFESTS:
        for data in records(load_manifest(directory / name), dylib_id):
            pin = ','.join(str(x) for x in data[:32])
            original, caller = struct.unpack('<2Q', data[32:])
            rows.append('{{' + pin + '},' + hex(original) + ',' + hex(caller) + '}')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        '/* Generated from supported host manifests; pins are checked at packaging. */\n'
        'struct renderer_binding { unsigned char pin[32]; uint64_t original; uint64_t caller; };\n'
        '__attribute__((used,section("__TEXT,__npa_renderer")))\n'
        'static const struct renderer_binding renderer_bindings[] = {\n'
        + ',\n'.join(rows) + '\n};\n'
        '_Static_assert(sizeof(struct renderer_binding) == 48, "renderer binding layout");\n'
    )


def verify_table(binary: object, manifest: Manifest | None, dylib_id: str) -> str:
    if manifest is None:
        raise ValueError('renderer verification requires a host manifest')
    section = binary.get_section(SECTION)
    if section is None or section.segment_name != '__TEXT':
        raise ValueError('renderer read-only binding section is missing')
    content = bytes(section.content)
    total = len(HOST_MANIFESTS) * len(records(manifest, dylib_id))
    if len(content) != total * RECORD_SIZE:
        raise ValueError('renderer binding table has an unexpected size')
    found = [
        content[i:i + RECORD_SIZE] for i in range(0, len(content), RECORD_SIZE)
    ]
    for row in records(manifest, dylib_id):
        if found.count(row) != 1:
            raise ValueError('renderer binding offsets do not match the host manifest')
    return 'host pin, original renderer and caller match'
