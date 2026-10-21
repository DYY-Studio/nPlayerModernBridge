import json
import subprocess
from pathlib import Path

import pytest
from npabridge import macho, verify
from npabridge.manifest import load_manifest
from npabridge.renderer_bindings import RENDERER_DYLIB_IDS

ROOT = Path(__file__).resolve().parents[1]
MANIFESTS = [load_manifest(ROOT / 'manifests' / name) for name in (
    'nplayer-3.13.0.json', 'nplayer-plus-3.13.0.json')]

# The five patches are the frozen sites for the standard build; Plus carries
# the mapped equivalents.
STANDARD_SITES = {
    0x1009A7098: (0x52800A02, 0x52800E62),
    0x1009A77AC: (0x52800A02, 0x52800E62),
    0x100B17730: (0x52860EC2, 0x52864602),
    0x100B17734: (0x72A68642, 0x72AF0682),
    0x100B81A88: (0x39452000, 0x52800020),
}
PLUS_SITES = {
    0x10018E6C4: (0x52800A02, 0x52800E62),
    0x10018EDD8: (0x52800A02, 0x52800E62),
    0x1002B5F78: (0x52860EC2, 0x52864602),
    0x1002B5F7C: (0x72A68642, 0x72AF0682),
    0x100320278: (0x39452000, 0x52800020),
}


def test_hdr_is_a_renderer_dylib():
    assert 'renderer-hdr' in RENDERER_DYLIB_IDS
    for manifest in MANIFESTS:
        dylib = manifest.dylib('renderer-hdr')
        assert dylib.basename == 'LibRendererHDRBridge.dylib'
        assert [api.symbol for domain in dylib.domains for api in domain.apis] == [
            'npa_renderer_shader_source']
        assert dylib.conflicts == ()


@pytest.mark.parametrize(
    'manifest,expected',
    list(zip(MANIFESTS, (STANDARD_SITES, PLUS_SITES))),
)
def test_hdr_patches(manifest, expected):
    sites = {site.site: (site.expected, site.replacement)
             for site in manifest.dylib('renderer-hdr').extra_sites}
    assert sites == expected
    api = manifest.dylib('renderer-hdr').domains[0].apis[0]
    assert len(api.call_sites) == 1


def test_hdr_closure_links_only_system_frameworks():
    closure = (ROOT / 'deps/renderer-hdr-closure.txt').read_text()
    values = dict(line.split('=', 1) for line in closure.splitlines())
    assert not values['project_archives'].strip()
    assert values['system_link_args'].split() == [
        '-framework', 'QuartzCore',
        '-framework', 'CoreGraphics',
        '-framework', 'Foundation',
    ]


def test_hdr_can_be_selected_with_highbit():
    for manifest in MANIFESTS:
        units = manifest.units(['libass', 'renderer-highbit', 'renderer-hdr'])
        assert [unit.dylib_id for unit in units] == [
            'libass', 'renderer-highbit', 'renderer-hdr']


def test_hdr_shader_rewrite(tmp_path):
    from npabridge.renderer_bindings import generate_header

    generate_header(
        ROOT / 'manifests', tmp_path / 'renderer-hdr-bindings.h', 'renderer-hdr')
    output = tmp_path / 'probe'
    subprocess.run([
        'xcrun', 'clang', '-O2', '-Werror=implicit-function-declaration',
        '-I' + str(tmp_path), str(ROOT / 'tests/renderer_hdr_probe.c'),
        '-framework', 'QuartzCore', '-framework', 'CoreGraphics',
        '-framework', 'CoreFoundation', '-framework', 'Foundation',
        '-o', str(output),
    ], check=True)
    subprocess.run([str(output)], check=True)


def test_hdr_bridge_verification():
    path = ROOT / 'build/LibRendererHDRBridge.dylib'
    if not path.is_file():
        pytest.skip('renderer-hdr bridge has not been built')
    for manifest in MANIFESTS:
        verify.verify_bridge(path, manifest.dylib('renderer-hdr'), manifest).require()
    assert macho.exported_symbols(macho.parse(path)) == ['_npa_renderer_shader_source']
