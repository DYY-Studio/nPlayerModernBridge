import subprocess
from pathlib import Path

import pytest
from npabridge import macho, verify
from npabridge.manifest import load_manifest

ROOT = Path(__file__).resolve().parents[1]
MANIFESTS = [load_manifest(ROOT / 'manifests' / name) for name in (
    'nplayer-3.13.0.json', 'nplayer-plus-3.13.0.json')]


def test_original_renderer_forwarding(tmp_path):
    from npabridge.renderer_bindings import generate_header
    generate_header(ROOT / 'manifests', tmp_path / 'renderer-bindings.h')
    output = tmp_path / 'probe'
    subprocess.run(['xcrun', 'clang', '-O2', '-Werror=implicit-function-declaration',
                    '-I' + str(ROOT / 'build/deps/ffmpeg-core/include'),
                    '-I' + str(tmp_path), str(ROOT / 'tests/renderer_bridge_probe.c'),
                    '-framework', 'CoreVideo', '-framework', 'CoreFoundation',
                    '-o', str(output)], check=True)
    subprocess.run([str(output)], check=True)


def test_renderer_binding_verification(tmp_path):
    path = ROOT / 'build/LibRendererHighBitBridge.dylib'
    for manifest in MANIFESTS:
        verify.verify_bridge(path, manifest.dylib('renderer-highbit'), manifest).require()
    binary = macho.parse(path)
    section = binary.get_section('__npa_renderer')
    assert section is not None
    raw = bytearray(path.read_bytes())
    raw[section.offset + 32] ^= 1
    changed = tmp_path / path.name
    changed.write_bytes(raw)
    report = verify.verify_bridge(changed, MANIFESTS[0].dylib('renderer-highbit'), MANIFESTS[0])
    assert 'bridge.renderer_bindings' in [c.code for c in report.failed]
    # A pre-fix bridge has no binding section and must also be rejected.
    raw = path.read_bytes()
    name = b'__npa_renderer\0\0'
    assert raw.count(name) == 1
    changed.write_bytes(raw.replace(name, b'__npa_removed\0\0\0'))
    report = verify.verify_bridge(changed, MANIFESTS[0].dylib('renderer-highbit'), MANIFESTS[0])
    assert 'bridge.renderer_bindings' in [c.code for c in report.failed]


def test_corrupt_renderer_prevents_packaging(tmp_path):
    from npabridge.patch import patch_ipa
    path = ROOT / 'build/LibRendererHighBitBridge.dylib'
    binary = macho.parse(path)
    section = binary.get_section('__npa_renderer')
    assert section is not None
    raw = bytearray(path.read_bytes())
    raw[section.offset + 32] ^= 1
    (tmp_path / path.name).write_bytes(raw)
    output = tmp_path / 'rejected.ipa'
    with pytest.raises(verify.VerificationError) as raised:
        patch_ipa(ROOT.parent / 'nPlayer_3.13.0.ipa', output, tmp_path,
                  ROOT / 'manifests', ('renderer-highbit',))
    assert 'bridge.renderer_bindings' in raised.value.codes
    assert not output.exists()
