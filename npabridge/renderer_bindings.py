"""Renderer host offsets generated and verified against supported manifests."""
from __future__ import annotations

import struct
from pathlib import Path

from .manifest import Manifest, load_manifest

SECTION = '__npa_renderer'
RECORD_SIZE = 48


def record(manifest: Manifest) -> bytes:
    api = manifest.api('npa_renderer_create_pixel_buffer')
    if len(api.call_sites) != 1:
        raise ValueError('renderer binding requires one frozen caller')
    return bytes.fromhex(manifest.main_pin_sha256) + struct.pack(
        '<2Q', api.old_target - manifest.imagebase,
        api.call_sites[0] + 4 - manifest.imagebase,
    )


def generate_header(directory: Path, output: Path) -> None:
    rows = []
    for name in ('nplayer-3.13.0.json', 'nplayer-plus-3.13.0.json'):
        data = record(load_manifest(directory / name))
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


def verify_table(binary: object, manifest: Manifest | None) -> str:
    if manifest is None:
        raise ValueError('renderer verification requires a host manifest')
    section = binary.get_section(SECTION)
    if section is None or section.segment_name != '__TEXT':
        raise ValueError('renderer read-only binding section is missing')
    content = bytes(section.content)
    if len(content) != 2 * RECORD_SIZE:
        raise ValueError('renderer binding table has an unexpected size')
    records = [content[i:i + RECORD_SIZE] for i in range(0, len(content), RECORD_SIZE)]
    if sum(r[:32] == bytes.fromhex(manifest.main_pin_sha256) for r in records) != 1:
        raise ValueError('renderer binding host pin is missing or duplicated')
    if record(manifest) not in records:
        raise ValueError('renderer binding offsets do not match the host manifest')
    return 'host pin, original renderer and caller match'
